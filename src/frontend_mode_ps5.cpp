/* PS5 RetroArch - which frontend a launch of eboot.bin starts.
 *
 * Copyright (C) 2026 Mihawk
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The title has a pre-screen that chooses between RetroArch and EmulationStation
 * (docs/FRONTENDS.md), and each runs in a process of its own: the title restarts
 * itself through LoadExec (approach B). eboot.bin is what the home screen starts,
 * so it decides first, before RetroArch sets anything up:
 *
 *   --ps5-mode=retroarch   RetroArch, here (the picker chose it)
 *   --ps5-mode=es-de       EmulationStation, /app0/es-de/es-de.bin
 *   --ps5-mode=picker      the picker, /app0/picker/picker.bin
 *   no mode                a launch from the home screen, or EmulationStation
 *                          quitting: the picker; but a test run's launch
 *                          (/app0/test-run.txt, which tools/run-title.sh writes)
 *                          stays RetroArch, as every test expects, unless a picker
 *                          test is armed (/app0/picker/picker-test.txt)
 *
 * Quitting a frontend goes back to the picker: EmulationStation restarts eboot.bin
 * with no mode (frontends/es-de/ps5/main_ps5.cpp), and RetroArch, when the picker
 * started it, does the same once it has quit and closed its drivers
 * (ps5_frontend_after_retroarch, from src/main.cpp). The picker's CIRCLE closes the
 * title. RetroArch started any other way (a test run) closes the title as before.
 *
 * A title built without the picker, or without EmulationStation, runs RetroArch
 * as it always has. LoadExec, accepted, returns and the shell replaces the process a
 * moment later (evidence/loadexec-relaunch): this waits for that, up to a minute. A
 * LoadExec refused, or a process still here after the wait, is recorded in the trace
 * and RetroArch runs instead, so a launch never ends on a black screen.
 */
#include "frontend_mode_ps5.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>

#include "trace.hpp"

extern "C" int sceSystemServiceLoadExec(const char *path, const char *const *argv);

namespace ps5::frontend_mode
{
namespace
{
/* This launch's --ps5-mode, kept for the quit */
std::string launch_mode;

bool exists(const std::string &path)
{
    std::FILE *file = std::fopen(path.c_str(), "rb");
    if (file)
        std::fclose(file);
    return file != nullptr;
}
} // namespace

std::string mode_argument(int argc, char **argv)
{
    static const char prefix[] = "--ps5-mode=";
    for (int i = 0; i < argc && argv && argv[i]; i++)
        if (std::strncmp(argv[i], prefix, sizeof(prefix) - 1) == 0)
            return argv[i] + sizeof(prefix) - 1;
    return "";
}

Next decide(const Launch &launch)
{
    if (launch.mode == "es-de")
        return launch.es_de_present ? Next::es_de : Next::retroarch;
    if (launch.mode == "picker")
        return launch.picker_present ? Next::picker : Next::retroarch;
    if (!launch.mode.empty())
        return Next::retroarch; /* retroarch, or a mode this build does not know */
    if (!launch.picker_present)
        return Next::retroarch;
    if (launch.picker_test)
        return Next::picker;
    return launch.test_run ? Next::retroarch : Next::picker;
}

const char *name(Next next)
{
    switch (next)
    {
    case Next::picker:
        return "picker";
    case Next::es_de:
        return "es-de";
    default:
        return "retroarch";
    }
}

void run(const Paths &paths, int argc, char **argv, unsigned replaced_wait_seconds)
{
    Launch launch;
    launch.mode = mode_argument(argc, argv);
    launch_mode = launch.mode;
    launch.test_run = exists(paths.test_run);
    launch.picker_test = exists(paths.picker_test);
    launch.picker_present = exists(paths.picker);
    launch.es_de_present = exists(paths.es_de);
    const Next next = decide(launch);
    char line[256];
    std::snprintf(line, sizeof line,
                  "frontend: mode '%s', test run %d, picker test %d, picker %d, es-de %d -> %s",
                  launch.mode.c_str(), launch.test_run, launch.picker_test, launch.picker_present,
                  launch.es_de_present, name(next));
    ps5::debug::mark(line);
    if (next == Next::retroarch)
        return;
    const std::string &image = next == Next::picker ? paths.picker : paths.es_de;
    const char *const arguments[] = {"", nullptr};
    std::fflush(nullptr);
    const int result = sceSystemServiceLoadExec(image.c_str(), arguments);
    if (result >= 0)
        for (unsigned waited = 0; waited < replaced_wait_seconds * 10; waited++)
            usleep(100000);
    /* Only a LoadExec that did not replace the process gets here. */
    ps5::debug::mark_value("frontend: LoadExec did not replace the process; RetroArch runs; result",
                           result);
}
bool back_to_picker(const std::string &mode, bool picker_present)
{
    return mode == "retroarch" && picker_present;
}

void after_retroarch(const Paths &paths, const std::string &mode, unsigned replaced_wait_seconds)
{
    if (!back_to_picker(mode, exists(paths.picker)))
        return;
    ps5::debug::mark("frontend: RetroArch quit; back to the picker");
    const char *const arguments[] = {"", nullptr};
    std::fflush(nullptr);
    const int result = sceSystemServiceLoadExec(paths.eboot.c_str(), arguments);
    if (result >= 0)
        for (unsigned waited = 0; waited < replaced_wait_seconds * 10; waited++)
            usleep(100000);
    ps5::debug::mark_value(
        "frontend: LoadExec did not replace the process; the title closes; result", result);
}
} // namespace ps5::frontend_mode

namespace
{
const ps5::frontend_mode::Paths title_paths{"/app0/picker/picker.bin", "/app0/es-de/es-de.bin",
                                            "/app0/test-run.txt", "/app0/picker/picker-test.txt",
                                            "/app0/eboot.bin"};
}

extern "C" void ps5_frontend_dispatch(int argc, char **argv)
{
    ps5::frontend_mode::run(title_paths, argc, argv, 60);
}

extern "C" void ps5_frontend_after_retroarch(void)
{
    ps5::frontend_mode::after_retroarch(title_paths, ps5::frontend_mode::launch_mode, 60);
}
