/* Copyright (C) 2026 Mihawk; SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
// The server owns no emulator state; saved WebUI settings load on next launch.
// apply_settings: the WebUI's pending core settings go into the cores' files now; only
// before RetroArch starts (eboot.bin), never from the daemon while a core may run.
bool ps5_webui_start(const char *root, unsigned short port = 6769, bool apply_settings = true);
void ps5_webui_stop();
// The title's side when the daemon serves (src/webui_link.h): the folder core option
// metadata is written to, and the saved core settings applied before RetroArch reads them.
void ps5_webui_prepare(const char *root);
// The frontend the title shows now, for the status ("picker", "retroarch", "es-de").
void ps5_webui_set_frontend(const char *frontend);
// Requests being answered now (an upload or a download among them).
unsigned ps5_webui_connections();

struct retro_core_options_v2;
struct retro_variable;
extern "C" void ps5_webui_core_options(const char *, const retro_core_options_v2 *);
extern "C" void ps5_webui_core_variables(const char *, const retro_variable *);
