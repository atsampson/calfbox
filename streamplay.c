/*
Calf Box, an open source musical instrument.
Copyright (C) 2010-2011 Krzysztof Foltman

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "assert.h"
#include "config.h"
#include "config-api.h"
#include "dspmath.h"
#include "module.h"
#include <errno.h>
#include <glib.h>
#include <jack/ringbuffer.h>
#include <malloc.h>
#include <math.h>
#include <memory.h>
#include <pthread.h>
#include <sndfile.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define CUE_BUFFER_SIZE 16000
#define PREFETCH_THRESHOLD (CUE_BUFFER_SIZE / 4)
#define MAX_READAHEAD_BUFFERS 3

#define NO_SAMPLE_LOOP ((uint64_t)-1ULL)

struct stream_player_cue_point
{
    volatile uint64_t position;
    volatile uint32_t size, length;
    float *data;
    int queued;
};

struct stream_state
{
    SNDFILE *sndfile;
    SF_INFO info;
    uint64_t readptr;
    uint64_t restart;
    
    volatile int buffer_in_use;
    
    struct stream_player_cue_point cp_start, cp_loop, cp_readahead[MAX_READAHEAD_BUFFERS];
    int cp_readahead_ready[MAX_READAHEAD_BUFFERS];
    struct stream_player_cue_point *pcp_current, *pcp_next;
    
    jack_ringbuffer_t *rb_for_reading, *rb_just_read;
    float gain;
};

struct stream_player_module
{
    struct cbox_module module;
    
    pthread_t thr_preload;
    
    struct stream_state *stream;
};

static void init_cue(struct stream_state *ss, struct stream_player_cue_point *pt, uint32_t size, uint64_t pos)
{
    pt->data = malloc(size * sizeof(float) * ss->info.channels);
    pt->size = size;
    pt->length = 0;
    pt->queued = 0;
    pt->position = pos;
}

static void load_at_cue(struct stream_state *ss, struct stream_player_cue_point *pt)
{
    if (pt->position != NO_SAMPLE_LOOP)
    {
        sf_seek(ss->sndfile, pt->position, 0);
        pt->length = sf_readf_float(ss->sndfile, pt->data, pt->size);
    }
    pt->queued = 0;
}

static int is_contained(struct stream_player_cue_point *pt, uint64_t ofs)
{
    return pt->position != NO_SAMPLE_LOOP && ofs >= pt->position && ofs < pt->position + pt->length;
}

static int is_queued(struct stream_player_cue_point *pt, uint64_t ofs)
{
    return pt->queued && pt->position != NO_SAMPLE_LOOP && ofs >= pt->position && ofs < pt->position + pt->size;
}

struct stream_player_cue_point *get_cue(struct stream_state *ss, uint64_t pos)
{
    int i;
    
    if (is_contained(&ss->cp_loop, pos))
        return &ss->cp_loop;
    if (is_contained(&ss->cp_start, pos))
        return &ss->cp_start;
    
    for (i = 0; i < MAX_READAHEAD_BUFFERS; i++)
    {
        if (ss->cp_readahead_ready[i] && is_contained(&ss->cp_readahead[i], pos))
            return &ss->cp_readahead[i];
    }
    return NULL;
}

struct stream_player_cue_point *get_queued_buffer(struct stream_state *ss, uint64_t pos)
{
    int i;
    
    for (i = 0; i < MAX_READAHEAD_BUFFERS; i++)
    {
        if (!ss->cp_readahead_ready[i] && is_queued(&ss->cp_readahead[i], pos))
            return &ss->cp_readahead[i];
    }
    return NULL;
}

void request_load(struct stream_state *ss, int buf_idx, uint64_t pos)
{
    unsigned char cidx = (unsigned char)buf_idx;
    struct stream_player_cue_point *pt = &ss->cp_readahead[buf_idx];
    int wlen = 0;
    
    ss->cp_readahead_ready[buf_idx] = 0;    
    pt->position = pos;
    pt->length = 0;
    pt->queued = 1;

    wlen = jack_ringbuffer_write(ss->rb_for_reading, &cidx, 1);
    assert(wlen);
}

int get_unused_buffer(struct stream_state *ss)
{
    int i = 0;
    int notbad = -1;
    
    // return first buffer that is not currently played or in queue; XXXKF this is a very primitive strategy, a good one would at least use the current play position
    for (i = 0; i < MAX_READAHEAD_BUFFERS; i++)
    {
        int64_t rel;
        if (&ss->cp_readahead[i] == ss->pcp_current)
            continue;
        if (ss->cp_readahead[i].queued)
            continue;
        // If there's any unused buffer, return it
        if (ss->cp_readahead[i].position == NO_SAMPLE_LOOP)
            return i;
        // If this has already been played, return it
        rel = ss->readptr - ss->cp_readahead[i].position;
        if (rel >= ss->cp_readahead[i].length)
            return i;
        // Use as second chance
        notbad = i;
    }
    return notbad;
}

void *sample_preload_thread(void *user_data)
{
    struct stream_player_module *m = user_data;
    struct stream_state *ss = m->stream;
    
    do {
        unsigned char buf_idx;
        if (!jack_ringbuffer_read(ss->rb_for_reading, &buf_idx, 1))
        {
            usleep(5000);
            continue;
        }
        if (buf_idx == 255)
            break;
        // fprintf(stderr, "Preload: %d, %lld\n", (int)buf_idx, (long long)m->cp_readahead[buf_idx].position);
        load_at_cue(ss, &ss->cp_readahead[buf_idx]);
        // fprintf(stderr, "Preloaded\n", (int)buf_idx, (long long)m->cp_readahead[buf_idx].position);
        jack_ringbuffer_write(ss->rb_just_read, &buf_idx, 1);
    } while(1);
        
}

void stream_player_process_event(struct cbox_module *module, const uint8_t *data, uint32_t len)
{
    struct stream_player_module *m = (struct stream_player_module *)module;
}

static void request_next(struct stream_state *ss, uint64_t pos)
{
    // Check if we've requested a next buffer, if not, request it
    
    // First verify if our idea of 'next' buffer is correct
    // XXXKF This is technically incorrect, it won't tell whether the next "block" that's there
    // isn't actually a single sample. I worked it around by ensuring end of blocks are always
    // at CUE_BUFFER_SIZE boundary, and this works well, but causes buffers to be of uneven size.
    if (ss->pcp_next && (is_contained(ss->pcp_next, pos) || is_queued(ss->pcp_next, pos)))
    {
        // We're still waiting for the requested buffer, but that's OK
        return;
    }
    
    // We don't know the next buffer, or the next buffer doesn't contain
    // the sample we're looking for.
    ss->pcp_next = get_queued_buffer(ss, pos);
    if (!ss->pcp_next)
    {
        // It hasn't even been requested - request it
        int buf_idx = get_unused_buffer(ss);
        if(buf_idx == -1)
        {
            printf("Ran out of buffers\n");
            return;
        }
        request_load(ss, buf_idx, pos);
        ss->pcp_next = &ss->cp_readahead[buf_idx];
        
        // printf("@%lld: Requested load into buffer %d at %lld\n", (long long)m->readptr, buf_idx, (long long) pos);
    }
}

static void copy_samples(struct stream_state *ss, cbox_sample_t **outputs, float *data, int count, int ofs, int pos)
{
    int i;
    float gain = ss->gain;
    
    if (ss->info.channels == 1)
    {
        for (i = 0; i < count; i++)
        {
            outputs[0][ofs + i] = outputs[1][ofs + i] = gain * data[pos + i];
        }
    }
    else
    if (ss->info.channels == 2)
    {
        for (i = 0; i < count; i++)
        {
            outputs[0][ofs + i] = gain * data[pos << 1];
            outputs[1][ofs + i] = gain * data[(pos << 1) + 1];
            pos++;
        }
    }
    else
    {
        uint32_t ch = ss->info.channels;
        for (i = 0; i < count; i++)
        {
            outputs[0][ofs + i] = gain * data[pos * ch];
            outputs[1][ofs + i] = gain * data[pos * ch + 1];
            pos++;
        }
    }
    ss->readptr += count;
    if (ss->readptr >= (uint32_t)ss->info.frames)
    {
        ss->readptr = ss->restart;
    }
}

void stream_player_process_block(struct cbox_module *module, cbox_sample_t **inputs, cbox_sample_t **outputs)
{
    struct stream_player_module *m = (struct stream_player_module *)module;
    struct stream_state *ss = m->stream;
    int i, optr;
    unsigned char buf_idx;
    
    if (ss->readptr == NO_SAMPLE_LOOP)
    {
        for (int i = 0; i < CBOX_BLOCK_SIZE; i++)
        {
            outputs[0][i] = outputs[1][i] = 0;
        }
        return;
    }

    // receive buffer completion messages from the queue
    while(jack_ringbuffer_read(ss->rb_just_read, &buf_idx, 1))
    {
        ss->cp_readahead_ready[buf_idx] = 1;
    }
    
    optr = 0;
    do {
        if (ss->readptr == NO_SAMPLE_LOOP)
            break;

        if (ss->pcp_current && !is_contained(ss->pcp_current, ss->readptr))
            ss->pcp_current = NULL;
        
        if (!ss->pcp_current)
        {
            if (ss->pcp_next && is_contained(ss->pcp_next, ss->readptr))
            {
                ss->pcp_current = ss->pcp_next;
                ss->pcp_next = NULL;
            }
            else
                ss->pcp_current = get_cue(ss, ss->readptr);
        }
        
        if (!ss->pcp_current)
        {
            printf("Underrun at %d\n", (int)ss->readptr);
            // Underrun; request/wait for next block and output zeros
            request_next(ss, ss->readptr);
            break;
        }
        assert(!ss->pcp_current->queued);
        
        uint64_t data_end = ss->pcp_current->position + ss->pcp_current->length;
        uint32_t data_left = data_end - ss->readptr;
        
        // If we're close to running out of space, prefetch the next bit
        if (data_left < PREFETCH_THRESHOLD && data_end < ss->info.frames)
            request_next(ss, data_end);
        
        float *data = ss->pcp_current->data;
        uint32_t pos = ss->readptr - ss->pcp_current->position;
        uint32_t count = data_end - ss->readptr;
        if (count > CBOX_BLOCK_SIZE - optr)
            count = CBOX_BLOCK_SIZE - optr;
        
        // printf("Copy samples: copying %d, optr %d, %lld = %d @ [%lld - %lld], left %d\n", count, optr, (long long)m->readptr, pos, (long long)m->pcp_current->position, (long long)data_end, (int)data_left);
        copy_samples(ss, outputs, data, count, optr, pos);
        optr += count;
    } while(optr < CBOX_BLOCK_SIZE);
    
    for (i = optr; i < CBOX_BLOCK_SIZE; i++)
    {
        outputs[0][i] = outputs[1][i] = 0;
    }
}

void stream_player_destroy(struct cbox_module *module)
{
    struct stream_player_module *m = (struct stream_player_module *)module;
    unsigned char cmd = 255;
    
    jack_ringbuffer_write(m->stream->rb_for_reading, &cmd, 1);
    pthread_join(m->thr_preload, NULL);
    
    jack_ringbuffer_free(m->stream->rb_for_reading);
    jack_ringbuffer_free(m->stream->rb_just_read);
    sf_close(m->stream->sndfile);
}

struct stream_state *create_stream(const char *context, const char *filename)
{
    struct stream_state *stream = malloc(sizeof(struct stream_state));
    stream->sndfile = sf_open(filename, SFM_READ, &stream->info);
    
    if (!stream->sndfile)
    {
        g_error("%s: cannot open file '%s': %s", context, filename, sf_strerror(NULL));
        return NULL;
    }
    g_message("Frames %d channels %d", (int)stream->info.frames, (int)stream->info.channels);
    
    stream->rb_for_reading = jack_ringbuffer_create(MAX_READAHEAD_BUFFERS + 1);
    stream->rb_just_read = jack_ringbuffer_create(MAX_READAHEAD_BUFFERS + 1);
    
    stream->readptr = 0;
    stream->restart = -1;
    stream->pcp_current = &stream->cp_start;
    stream->pcp_next = NULL;
    stream->gain = 1.0;
    
    init_cue(stream, &stream->cp_start, CUE_BUFFER_SIZE, 0);
    load_at_cue(stream, &stream->cp_start);
    if (stream->restart > 0 && (stream->restart % CUE_BUFFER_SIZE) > 0)
        init_cue(stream, &stream->cp_loop, CUE_BUFFER_SIZE + (CUE_BUFFER_SIZE - (stream->restart % CUE_BUFFER_SIZE)), stream->restart);
    else
        init_cue(stream, &stream->cp_loop, CUE_BUFFER_SIZE, stream->restart);
    load_at_cue(stream, &stream->cp_loop);
    for (int i = 0; i < MAX_READAHEAD_BUFFERS; i++)
    {
        init_cue(stream, &stream->cp_readahead[i], CUE_BUFFER_SIZE, NO_SAMPLE_LOOP);
        stream->cp_readahead_ready[i] = 0;
    }
    return stream;
}

struct cbox_module *stream_player_create(void *user_data, const char *cfg_section, int srate)
{
    int rest;
    static int inited = 0;
    
    if (!inited)
    {
        inited = 1;
    }
    
    struct stream_player_module *m = malloc(sizeof(struct stream_player_module));
    char *filename = cbox_config_get_string(cfg_section, "file");
    if (!filename)
    {
        g_error("%s: filename not specified", cfg_section);
        return NULL;
    }
    cbox_module_init(&m->module, m);
    m->module.process_event = stream_player_process_event;
    m->module.process_block = stream_player_process_block;
    m->module.destroy = stream_player_destroy;
    m->stream = create_stream(cfg_section, filename);
    m->stream->restart = (uint64_t)(int64_t)cbox_config_get_int(cfg_section, "loop", m->stream->restart);
    m->stream->gain = cbox_config_get_gain(cfg_section, "gain", m->stream->gain);
    
    if (pthread_create(&m->thr_preload, NULL, sample_preload_thread, m))
    {
        g_error("Failed to create audio prefetch thread", strerror(errno));
        return NULL;
    }
    
    
    return &m->module;
}

struct cbox_module_keyrange_metadata stream_player_keyranges[] = {
};

struct cbox_module_livecontroller_metadata stream_player_controllers[] = {
};

DEFINE_MODULE(stream_player, 0, 2)

