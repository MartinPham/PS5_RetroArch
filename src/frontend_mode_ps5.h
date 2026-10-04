/* PS5 RetroArch - which frontend a launch of eboot.bin starts (src/frontend_mode_ps5.cpp says why).
 *
 * Copyright (C) 2026 Mihawk
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef PS5_RETROARCH_FRONTEND_MODE_PS5_H
#define PS5_RETROARCH_FRONTEND_MODE_PS5_H

#include "ps5_game.h"

#ifdef __cplusplus
#include <string>

namespace ps5::frontend_mode
{
enum class Next
{
    retroarch, /* RetroArch, in this process */
    game,      /* a frontend's game, in RetroArch in this process (src/ps5_game.h) */
    picker,    /* the frontend picker, /app0/picker/picker.bin */
    es_de      /* EmulationStation, /app0/es-de/es-de.bin */
};

struct Launch
{
    std::string mode;            /* --ps5-mode=<mode>, or empty */
    bool test_run = false;       /* a test run's launch (/app0/test-run.txt) */
    bool picker_test = false;    /* an armed picker test (/app0/picker/picker-test.txt) */
    bool picker_present = false; /* the title carries the picker */
    bool es_de_present = false;  /* the title carries EmulationStation */
};

struct Paths
{
    std::string picker, es_de, test_run, picker_test, eboot;
    std::string request, result, playlists; /* game mode's (src/ps5_game.h) */
};

/* The mode the process arguments name with --ps5-mode=, from argv[0] on (LoadExec's
 * arguments are the whole argv), or an empty string. */
std::string mode_argument(int argc, char **argv);
Next decide(const Launch &launch);
const char *name(Next next);
/* Starts what this launch is for. It returns only when RetroArch runs in this
 * process: chosen, a game it runs, or a LoadExec refused or that did not replace the
 * process within replaced_wait_seconds. */
void run(const Paths &paths, int argc, char **argv, unsigned replaced_wait_seconds);
/* In game mode, the game RetroArch is to run; nullptr otherwise. */
const struct ps5_game *running_game();
/* After RetroArch has quit: true when the title goes back to the picker instead of
 * closing, which it does when the picker started RetroArch (--ps5-mode=retroarch). */
bool back_to_picker(const std::string &mode, bool picker_present);
/* After RetroArch has quit with status: in game mode, the result for the frontend and
 * a restart as it; else the picker, when back_to_picker says so. It returns only when
 * neither applies, or when LoadExec did not replace the process. */
void after_retroarch(const Paths &paths, const std::string &mode, int status,
                     unsigned replaced_wait_seconds);
} // namespace ps5::frontend_mode

extern "C"
{
#endif

    void ps5_frontend_dispatch(int argc, char **argv);
    /* In game mode, the game RetroArch runs (its core and content); NULL otherwise. */
    const struct ps5_game *ps5_frontend_game(void);
    /* Called once RetroArch has quit with status, before the title closes. */
    void ps5_frontend_after_retroarch(int status);

#ifdef __cplusplus
}
#endif

#endif
