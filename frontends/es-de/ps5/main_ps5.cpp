/* PS5 RetroArch - EmulationStation's entry point on the PS5.
 *
 * Copyright (C) 2026 Mihawk
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * ES-DE runs as its own executable of the title, /app0/es-de/es-de.bin, started
 * by eboot.bin through LoadExec (docs/FRONTENDS.md). Its own main (renamed
 * esde_main for this port, patches/0001) expects what a desktop gives it, which
 * a PS5 process does not have:
 *
 *  - argv[0] is the program: on the PS5 LoadExec's arguments are the whole argv,
 *    so this builds one with /app0/es-de/es-de.bin first, from which ES-DE
 *    finds its resources (/app0/es-de/resources; it has no /proc/self/exe here);
 *  - $HOME names its data directory: /app0/es-de, so its settings, game lists
 *    and themes are /app0/es-de/ES-DE, inside the title's folder;
 *  - a working directory getcwd can name: /app0/es-de again
 *    (frontends/common/frontend_shims.c), since ES-DE makes every path absolute
 *    against it.
 *
 * The OpenGL driver's own reports (its printf lines: presentation, batching,
 * glthread) go to /app0/es-de/stdout.txt, a line at a time. /app0/es-de/env.txt,
 * when present, sets environment variables before ES-DE starts, one NAME=VALUE a
 * line: the driver's switches (PS5_GLTHREAD=1 runs Mesa's GL thread) can be tried
 * on the console without a new build.
 *
 * A game ES-DE starts runs in RetroArch through the title's game mode, and the title
 * comes back here when it is closed (game_ps5.cpp says how).
 *
 * When ES-DE returns (Quit in its menu), the title goes back to eboot.bin, with
 * the arguments of this port that this run was given (a relaunch test's, so the
 * test sees its next generation).
 */
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

int esde_main(int argc, char *argv[]);

extern "C" int sceSystemServiceLoadExec(const char *path, const char *const *argv);
extern "C" int sceSystemServiceHideSplashScreen(void);
extern "C" int sceKernelUsleep(unsigned int microseconds);
extern "C" int ps5_frontend_chdir(const char *path);
extern "C" int ps5_esde_take_game_result(void);

/* ../PS5_OpenGL's heap (native-app/app_heap.c), which every allocation of this
 * program goes through: its 128 MiB default refused the Alekfull NX theme's
 * 1920x1080 backgrounds (8 MiB each decoded, one a system) at about 120 MiB live
 * and operator new trapped (klog/run-PPSA99169-162412.log). A size over 128 MiB is
 * mapped from direct memory, of which a title has about 12 GiB. */
extern "C" const size_t ps5_opengl_heap_size = size_t(1) << 30;

namespace
{
constexpr const char *program = "/app0/es-de/es-de.bin";
constexpr const char *home = "/app0/es-de";
constexpr const char *start_log = "/app0/es-de/es-de-ps5.log";

void note(const char *what, int value)
{
    if (std::FILE *log = std::fopen(start_log, "a"))
    {
        std::fprintf(log, "%lld %s %d\n", static_cast<long long>(std::time(nullptr)), what, value);
        std::fclose(log);
    }
}
} // namespace

int main(int argc, char **argv)
{
    note("start: hide splash", sceSystemServiceHideSplashScreen());
    if (std::freopen("/app0/es-de/stdout.txt", "w", stdout))
        std::setvbuf(stdout, nullptr, _IOLBF, 0);
    setenv("HOME", home, 1);
    if (std::FILE *environment = std::fopen("/app0/es-de/env.txt", "r"))
    {
        char line[256];
        int set = 0;
        while (std::fgets(line, sizeof(line), environment))
        {
            line[std::strcspn(line, "\r\n")] = '\0';
            char *equals = std::strchr(line, '=');
            if (line[0] == '#' || !equals || equals == line)
                continue;
            *equals = '\0';
            set += setenv(line, equals + 1, 1) == 0 ? 1 : 0;
        }
        std::fclose(environment);
        note("start: variables from env.txt", set);
    }
    note("start: working directory (errno if refused)", ps5_frontend_chdir(home) == 0 ? 0 : errno);
    /* Back from a game RetroArch ran (game mode, game_ps5.cpp)? ES-DE's start asks. */
    note("start: back from a game", ps5_esde_take_game_result());

    std::vector<std::string> port_arguments;
    std::vector<char *> arguments{const_cast<char *>(program)};
    for (int i = 0; i < argc && argv && argv[i]; i++)
    {
        if (std::strncmp(argv[i], "--ps5-", 6) == 0)
            port_arguments.emplace_back(argv[i]);
        else if (argv[i][0] != '\0')
            arguments.push_back(argv[i]);
    }
    note("start: arguments for ES-DE", static_cast<int>(arguments.size()) - 1);
    arguments.push_back(nullptr);

    const int status = esde_main(static_cast<int>(arguments.size()) - 1, arguments.data());
    note("ES-DE returned", status);

    std::vector<const char *> back;
    for (const std::string &argument : port_arguments)
        back.push_back(argument.c_str());
    if (back.empty())
        back.push_back("");
    back.push_back(nullptr);
    note("back to eboot.bin: LoadExec", sceSystemServiceLoadExec("/app0/eboot.bin", back.data()));
    for (;;)
        sceKernelUsleep(100000);
}
