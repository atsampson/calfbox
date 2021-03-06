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

#include "config.h"
#include "config-api.h"
#include "dspmath.h"
#include "module.h"
#include "onepole-float.h"
#include <glib.h>
#include <malloc.h>
#include <math.h>
#include <memory.h>
#include <sndfile.h>
#include <stdio.h>
#include <stdlib.h>

#define NO_STAGES 12

#define MODULE_PARAMS phaser_params

struct phaser_params
{
    float center;
    float mdepth;
    float fb_amt;
    float lfo_freq;
    float sphase;
    float wet_dry;
    int stages;
};

#if USE_NEON
#include <arm_neon.h>

struct cbox_onepolef_stereo_state_neon {
    float32x2_t x1;
    float32x2_t y1;
};
#endif

struct phaser_module
{
    struct cbox_module module;

#if USE_NEON
    struct cbox_onepolef_stereo_state_neon state[NO_STAGES];
#else
    struct cbox_onepolef_state state[NO_STAGES][2];
#endif
    struct cbox_onepolef_coeffs coeffs[2];
    float fb[2];
    float tpdsr;
    struct phaser_params *params;
    
    float phase;
};

gboolean phaser_process_cmd(struct cbox_command_target *ct, struct cbox_command_target *fb, struct cbox_osc_command *cmd, GError **error)
{
    struct phaser_module *m = (struct phaser_module *)ct->user_data;
    
    EFFECT_PARAM("/center_freq", "f", center, double, , 10, 20000) else
    EFFECT_PARAM("/mod_depth", "f", mdepth, double, , 0, 10000) else
    EFFECT_PARAM("/fb_amt", "f", fb_amt, double, , -1, 1) else
    EFFECT_PARAM("/lfo_freq", "f", lfo_freq, double, , 0, 20) else
    EFFECT_PARAM("/stereo_phase", "f", sphase, double, deg2rad, 0, 360) else
    EFFECT_PARAM("/wet_dry", "f", wet_dry, double, , 0, 1) else
    EFFECT_PARAM("/stages", "i", stages, int, , 1, 12) else
    if (!strcmp(cmd->command, "/status") && !strcmp(cmd->arg_types, ""))
    {
        if (!cbox_check_fb_channel(fb, cmd->command, error))
            return FALSE;
        return cbox_execute_on(fb, NULL, "/center_freq", "f", error, m->params->center) &&
            cbox_execute_on(fb, NULL, "/mod_depth", "f", error, m->params->mdepth) &&
            cbox_execute_on(fb, NULL, "/fb_amt", "f", error, m->params->fb_amt) &&
            cbox_execute_on(fb, NULL, "/lfo_freq", "f", error, m->params->lfo_freq) &&
            cbox_execute_on(fb, NULL, "/stereo_phase", "f", error, rad2deg(m->params->sphase)) &&
            cbox_execute_on(fb, NULL, "/wet_dry", "f", error, m->params->wet_dry) &&
            cbox_execute_on(fb, NULL, "/stages", "i", error, m->params->stages) && 
            CBOX_OBJECT_DEFAULT_STATUS(&m->module, fb, error);
    }
    else
        return cbox_object_default_process_cmd(ct, fb, cmd, error);
    return TRUE;
}

void phaser_process_event(struct cbox_module *module, const uint8_t *data, uint32_t len)
{
    // struct phaser_module *m = (struct phaser_module *)module;
}

static inline float clip_w(float w)
{
    if (w > 0.9 * M_PI)
        return 0.9 * M_PI;
    return w;
}

void phaser_process_block(struct cbox_module *module, cbox_sample_t **inputs, cbox_sample_t **outputs)
{
    struct phaser_module *m = (struct phaser_module *)module;
    struct phaser_params *p = m->params;
    int s, i;
    int stages = p->stages;
    float fb_amt = p->fb_amt;
    if (stages < 0 || stages > NO_STAGES)
        stages = 0;

    if (p->mdepth)
    {
        cbox_onepolef_set_allpass(&m->coeffs[0], clip_w(m->tpdsr * p->center * cent2factor(p->mdepth * sin(m->phase))));
        cbox_onepolef_set_allpass(&m->coeffs[1], clip_w(m->tpdsr * p->center * cent2factor(p->mdepth * sin(m->phase + p->sphase))));
    }
    else
    {
        cbox_onepolef_set_allpass(&m->coeffs[0], m->tpdsr * p->center);
        cbox_onepolef_set_allpass(&m->coeffs[1], m->tpdsr * p->center);
    }
    m->phase += p->lfo_freq * CBOX_BLOCK_SIZE * m->tpdsr;
    
#if USE_NEON
    float *__restrict input1 = inputs[0];
    float *__restrict input2 = inputs[1];
    float *__restrict output1 = outputs[0];
    float *__restrict output2 = outputs[1];
    float32x2_t wetdry = {p->wet_dry, p->wet_dry};
    float32x2_t fb_amt2 = {fb_amt, fb_amt};
    float32x2_t fb = {m->fb[0], m->fb[1]};
    float32x2_t a0 = {m->coeffs[0].a0, m->coeffs[1].a0};
    float32x2_t a1 = {m->coeffs[0].a1, m->coeffs[1].a1};
    float32x2_t b1 = {m->coeffs[0].b1, m->coeffs[1].b1};
    float32x2_t zero = {0.f, 0.f};
    const float32x2_t thresh = {(1.0 / (65536.0 * 65536.0)), (1.0 / (65536.0 * 65536.0))};
    for (i = 0; i < CBOX_BLOCK_SIZE; i++)
    {
        float32x2_t dry = {input1[i], input2[i]};
        float32x2_t wet = vsub_f32(dry, vmul_f32(fb, fb_amt2));
        for (s = 0; s < (stages & ~1); s += 2)
        {
            // wet = sanef(coeffs->a0 * wet + coeffs->a1 * state->x1 - coeffs->b1 * state->y1);
            float32x2_t pre = wet;
            wet = vmla_f32(vmls_f32(vmul_f32(a1, m->state[s].x1), b1, m->state[s].y1), a0, wet);
            m->state[s].x1 = pre;
            m->state[s].y1 = wet;
            pre = wet;
            wet = vmla_f32(vmls_f32(vmul_f32(a1, m->state[s + 1].x1), b1, m->state[s + 1].y1), a0, wet);
            m->state[s + 1].x1 = pre;
            m->state[s + 1].y1 = wet;
        }
        for (; s < stages; s++)
        {
            // wet = sanef(coeffs->a0 * wet + coeffs->a1 * state->x1 - coeffs->b1 * state->y1);
            float32x2_t pre = wet;
            wet = vmla_f32(vmls_f32(vmul_f32(a1, m->state[s].x1), b1, m->state[s].y1), a0, wet);
            m->state[s].x1 = pre;
            m->state[s].y1 = wet;
        }
        fb = wet;
        wet = vadd_f32(dry, vmul_f32(vsub_f32(wet, dry), wetdry));
        output1[i] = wet[0];
        output2[i] = wet[1];
    }
    // set values < threshold to zero
    for (s = 0; s < stages; s++)
        m->state[s].y1 = vbsl_f32(vcage_f32(m->state[s].y1, thresh), m->state[s].y1, zero);
    m->fb[0] = fb[0];
    m->fb[1] = fb[1];
#else
    for (int c = 0; c < 2; c++)
    {
        for (i = 0; i < CBOX_BLOCK_SIZE; i++)
        {
            float dry = inputs[c][i];
            float wet = dry - m->fb[c] * fb_amt;
            for (s = 0; s < stages; s++)
                wet = cbox_onepolef_process_sample(&m->state[s][c], &m->coeffs[c], wet);
            m->fb[c] = wet;
            outputs[c][i] = dry + (wet - dry) * p->wet_dry;
        }
    }
#endif
}

MODULE_SIMPLE_DESTROY_FUNCTION(phaser)

MODULE_CREATE_FUNCTION(phaser)
{
    int b;
    
    static int inited = 0;
    if (!inited)
    {
        inited = 1;
    }
    
    struct phaser_module *m = malloc(sizeof(struct phaser_module));
    CALL_MODULE_INIT(m, 2, 2, phaser);
    m->module.process_event = phaser_process_event;
    m->module.process_block = phaser_process_block;
    m->tpdsr = 2.0 * M_PI / m->module.srate;
    m->phase = 0;
    struct phaser_params *p = malloc(sizeof(struct phaser_params));
    m->params = p;
    p->sphase = deg2rad(cbox_config_get_float(cfg_section, "stereo_phase", 90.f));
    p->lfo_freq = cbox_config_get_float(cfg_section, "lfo_freq", 1.f);
    p->center = cbox_config_get_float(cfg_section, "center_freq", 1500.f);
    p->mdepth = cbox_config_get_float(cfg_section, "mod_depth", 1200.f);
    p->fb_amt = cbox_config_get_float(cfg_section, "feedback", 0.f);
    p->wet_dry = cbox_config_get_float(cfg_section, "wet_dry", 0.5f);
    p->stages = cbox_config_get_int(cfg_section, "stages", NO_STAGES);
    
#if USE_NEON
    for (b = 0; b < NO_STAGES; b++)
    {
        float32x2_t zero = {0.f, 0.f};
        m->state[b].x1 = zero;
        m->state[b].y1 = zero;
    }
#else
    for (b = 0; b < NO_STAGES; b++)
        for (int c = 0; c < 2; c++)
            cbox_onepolef_reset(&m->state[b][c]);
#endif

    return &m->module;
}


struct cbox_module_keyrange_metadata phaser_keyranges[] = {
};

struct cbox_module_livecontroller_metadata phaser_controllers[] = {
};

DEFINE_MODULE(phaser, 2, 2)

