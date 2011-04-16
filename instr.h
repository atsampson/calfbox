/*
Calf Box, an open source musical instrument.
Copyright (C) 2010 Krzysztof Foltman

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

struct cbox_module;
struct cbox_io;

struct cbox_instrument
{
    struct cbox_module *module;
    struct cbox_module *insert;
};

extern void cbox_instruments_init(struct cbox_io *io);
extern struct cbox_instrument *cbox_instruments_get_by_name(const char *name);
extern struct cbox_io *cbox_instruments_get_io();
extern void cbox_instruments_close();