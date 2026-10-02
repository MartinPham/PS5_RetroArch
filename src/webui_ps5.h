/* Copyright (C) 2026 Mihawk; SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
// The server owns no emulator state; saved WebUI settings load on next launch.
bool ps5_webui_start(const char *root, unsigned short port = 6769);
void ps5_webui_stop();
