/* The game library every frontend shows (src/ps5_library.c), on the host: a library
 * shaped like the base console's (a mixed playlist whose entries name no core beside
 * one-system playlists with their cores), a playlist RetroArch scanned against its
 * databases, its history left out, a game in two playlists, a system of its own, the
 * cores and their info, and the launch commands game mode parses back.
 * argv[1] is a scratch folder. */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "../src/ps5_game.h"
#include "../src/ps5_library.h"

int sceSystemServiceLoadExec(const char *path, const char *const *argv)
{
    (void)path;
    (void)argv;
    return -1;
}

static void write_text(const char *path, const char *text)
{
    FILE *file = fopen(path, "wb");
    assert(file);
    fputs(text, file);
    fclose(file);
}

static void core(const char *dir, const char *name, const char *database, const char *extensions)
{
    char path[2048], text[2048];
    snprintf(path, sizeof(path), "%s/cores/%s_libretro.so", dir, name);
    write_text(path, "");
    snprintf(path, sizeof(path), "%s/info/%s_libretro.info", dir, name);
    snprintf(text, sizeof(text),
             "display_name = \"%s\"\ncorename = \"%s\"\ndatabase_match_archive_member = \"false\"\n"
             "database = \"%s\"\nsupported_extensions = \"%s\"\n",
             name, name, database, extensions);
    write_text(path, text);
}

static const struct ps5_library_system *system_of(const struct ps5_library *library, const char *id)
{
    for (size_t i = 0; i < library->system_count; i++)
        if (strcmp(library->systems[i].id, id) == 0)
            return &library->systems[i];
    return NULL;
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    const char *dir = argv[1];

    /* Platforms, by the names RetroArch and people give them. */
    assert(strcmp(ps5_library_platform("Nintendo - Super Nintendo Entertainment System.lpl"),
                  "snes") == 0);
    assert(strcmp(ps5_library_platform("Nintendo - Nintendo Entertainment System"), "nes") == 0);
    assert(strcmp(ps5_library_platform("Nintendo - Game Boy Color"), "gbc") == 0);
    assert(strcmp(ps5_library_platform("Nintendo - Game Boy"), "gb") == 0);
    assert(strcmp(ps5_library_platform("PS1"), "psx") == 0);
    assert(strcmp(ps5_library_platform("PS2.lpl"), "ps2") == 0);
    assert(strcmp(ps5_library_platform("Sony - PlayStation Portable"), "psp") == 0);
    assert(strcmp(ps5_library_platform("Nintendo 3DS.lpl"), "n3ds") == 0);
    assert(strcmp(ps5_library_platform("ARCADE - NeoGeo"), "neogeo") == 0);
    assert(strcmp(ps5_library_platform("ARCADE - CPS I, II & III"), "cps") == 0);
    assert(strcmp(ps5_library_platform("ARCADE - Others (FBNeo)"), "fbneo") == 0);
    assert(strcmp(ps5_library_platform("ARCADE - Sega System 16 & 32"), "arcade") == 0);
    assert(strcmp(ps5_library_platform("Genesis"), "genesis") == 0);
    assert(strcmp(ps5_library_platform("Sega - Mega Drive - Genesis"), "genesis") == 0);
    assert(!ps5_library_platform("content.lpl") && !ps5_library_platform("My Homebrew"));
    assert(strcmp(ps5_library_platform_database("psx"), "Sony - PlayStation") == 0);

    /* The library. */
    char path[2048];
    const char *folders[] = {"cores", "info", "playlists"};
    for (int i = 0; i < 3; i++)
    {
        snprintf(path, sizeof(path), "%s/%s", dir, folders[i]);
        mkdir(path, 0777);
    }
    core(dir, "snes9x", "Nintendo - Super Nintendo Entertainment System|Nintendo - Satellaview",
         "smc|sfc");
    core(dir, "mgba", "Nintendo - Game Boy|Nintendo - Game Boy Color|Nintendo - Game Boy Advance",
         "gb|gbc|gba");
    core(dir, "mednafen_psx_hw", "Sony - PlayStation", "cue|bin|chd");
    core(dir, "pcsx2", "Sony - PlayStation 2", "iso|chd");
    core(dir, "fbneo", "FBNeo - Arcade Games", "zip|7z");
    core(dir, "mame", "MAME", "zip|7z");
    snprintf(path, sizeof(path), "%s/info/missing_libretro.info", dir); /* info without a core */
    write_text(path, "display_name = \"missing\"\ndatabase = \"Sega - Saturn\"\n");

    snprintf(path, sizeof(path), "%s/playlists/content.lpl", dir);
    write_text(path,
               "{\"version\": \"1.5\", \"default_core_path\": \"\", \"items\": [\n"
               "{\"path\": \"/app0/content/Nintendo - Super Nintendo Entertainment System/Donkey "
               "Kong 2 (USA).zip\","
               " \"label\": \"Donkey Kong 2 (USA)\", \"core_path\": \"DETECT\", \"db_name\": "
               "\"content.lpl\"},\n"
               "{\"path\": \"/app0/content/PS1/Crash (USA).cue\", \"label\": \"Crash (USA)\","
               " \"core_path\": \"DETECT\", \"db_name\": \"content.lpl\"},\n"
               "{\"path\": \"/app0/content/PS1/Crash (USA).bin\", \"label\": \"\","
               " \"core_path\": \"DETECT\", \"db_name\": \"content.lpl\"},\n"
               "{\"path\": \"/app0/content/Nintendo - Game Boy/Tetris (World).zip\", \"label\": "
               "\"Tetris\","
               " \"core_path\": \"DETECT\", \"db_name\": \"content.lpl\"},\n"
               "{\"path\": \"/app0/content/ARCADE - CPS I, II & III/sf2.zip\", \"label\": \"sf2\","
               " \"core_path\": \"DETECT\", \"db_name\": \"content.lpl\"},\n"
               "{\"path\": \"/app0/content/Homebrew/demo.bin\", \"label\": \"Demo\","
               " \"core_path\": \"DETECT\", \"db_name\": \"content.lpl\"}\n]}\n");
    snprintf(path, sizeof(path), "%s/playlists/PS2.lpl", dir);
    snprintf(path + 1024, 1024, "%s/cores/pcsx2_libretro.so", dir);
    {
        char text[4096];
        snprintf(text, sizeof(text),
                 "{\"default_core_path\": \"%s\", \"items\": ["
                 "{\"path\": \"/app0/content/PS2/GoW II/God of War II (USA).iso\", \"label\": "
                 "\"God of War II\","
                 " \"core_path\": \"%s\", \"db_name\": \"PS2.lpl\"},"
                 "{\"path\": \"/app0/content/PS2/Okami (USA).iso\", \"label\": \"Okami\","
                 " \"core_path\": \"DETECT\", \"db_name\": \"PS2.lpl\"}]}",
                 path + 1024, path + 1024);
        write_text(path, text);
    }
    snprintf(path, sizeof(path), "%s/playlists/Nintendo - Super Nintendo Entertainment System.lpl",
             dir);
    write_text(path, "{\"items\": [{\"path\": \"/app0/content/Nintendo - Super Nintendo "
                     "Entertainment System/Donkey Kong "
                     "2 (USA).zip\", \"label\": \"DKC2 again\", \"db_name\": \"Nintendo - Super "
                     "Nintendo Entertainment "
                     "System.lpl\"}, {\"path\": \"/app0/content/Nintendo - Super Nintendo "
                     "Entertainment System/It's Mario "
                     "(USA).sfc\", \"label\": \"It's Mario\", \"core_path\": \"DETECT\", "
                     "\"db_name\": \"Nintendo - Super "
                     "Nintendo Entertainment System.lpl\"}]}");
    snprintf(path, sizeof(path), "%s/playlists/content_history.lpl", dir);
    write_text(path, "{\"items\": [{\"path\": \"/app0/content/elsewhere/history.zip\", "
                     "\"core_path\": \"DETECT\"}]}");

    /* What RetroArch remembers of play: favourites and history in builtin/, and a
     * runtime log per core and game. */
    snprintf(path, sizeof(path), "%s/playlists/builtin", dir);
    mkdir(path, 0777);
    snprintf(path, sizeof(path), "%s/playlists/builtin/content_favorites.lpl", dir);
    write_text(path, "{\"items\": [{\"path\": \"/app0/content/Nintendo - Super Nintendo "
                     "Entertainment System/It's Mario (USA).sfc\", \"core_path\": \"DETECT\"},"
                     "{\"path\": \"/app0/content/not/in/the/library.zip\"}]}");
    char history[2048];
    snprintf(history, sizeof(history), "%s/playlists/builtin/content_history.lpl", dir);
    write_text(history, "{\"items\": [{\"path\": \"/app0/content/PS2/Okami (USA).iso\"},"
                        "{\"path\": \"/app0/content/PS1/Crash (USA).cue\"},"
                        "{\"path\": \"/app0/content/Nintendo - Super Nintendo Entertainment "
                        "System/It's Mario (USA).sfc\"},"
                        "{\"path\": \"/app0/content/PS2/Okami (USA).iso\"}]}");
    snprintf(path, sizeof(path), "%s/playlists/logs", dir);
    mkdir(path, 0777);
    snprintf(path, sizeof(path), "%s/playlists/logs/snes9x", dir);
    mkdir(path, 0777);
    snprintf(path, sizeof(path), "%s/playlists/logs/snes9x/It's Mario (USA).lrtl", dir);
    write_text(path,
               "{\n  \"version\": \"1.0\",\n  \"runtime\": \"1:05:07\",\n  \"last_played\": "
               "\"2026-10-01 10:00:00\",\n  \"play_count\": \"3\",\n  \"state_slot\": \"0\"\n}\n");

    char playlists[1024], info[1024], cores[1024];
    snprintf(playlists, sizeof(playlists), "%s/playlists", dir);
    snprintf(info, sizeof(info), "%s/info", dir);
    snprintf(cores, sizeof(cores), "%s/cores", dir);
    struct ps5_library library;
    assert(ps5_library_load(&library, playlists, info, cores) == 0);
    assert(library.core_count == 6); /* the info without its core left out */
    assert(library.game_count == 9); /* DKC2 once, the history left out */
    assert(library.system_count == 6);

    const struct ps5_library_system *snes = system_of(&library, "snes");
    assert(snes && snes->known && snes->game_count == 2);
    assert(strcmp(snes->name, "Nintendo - Super Nintendo Entertainment System") == 0);
    snprintf(path, sizeof(path), "%s/snes9x_libretro.so", cores);
    assert(strcmp(snes->core, path) == 0); /* from the core info: no entry names one */
    assert(strcmp(snes->folder, "/app0/content/Nintendo - Super Nintendo Entertainment System") ==
           0);
    assert(strstr(snes->extensions, "zip") && strstr(snes->extensions, "sfc") &&
           strstr(snes->extensions, "smc"));
    const struct ps5_library_game *first = &library.games[snes->first_game];
    /* Playlists are read in name order: a game in two is the first one's. */
    assert(strcmp(first->label, "DKC2 again") == 0 &&
           strcmp(first->playlist, "Nintendo - Super Nintendo Entertainment System.lpl") == 0);
    assert(strcmp(library.games[snes->first_game + 1].label, "It's Mario") == 0);

    const struct ps5_library_system *psx = system_of(&library, "psx");
    assert(psx && psx->game_count == 2 && strcmp(psx->folder, "/app0/content/PS1") == 0);
    assert(strcmp(library.games[psx->first_game].label, "Crash (USA)") ==
           0); /* an empty label: the file's */

    const struct ps5_library_system *ps2 = system_of(&library, "ps2");
    assert(ps2 && ps2->game_count == 2 && strcmp(ps2->core, path + 1024) == 0);
    assert(strcmp(ps2->folder, "/app0/content/PS2") == 0); /* one game in a sub-folder */
    for (size_t i = 0; i < ps2->game_count; i++)
        assert(strcmp(library.games[ps2->first_game + i].core, ps2->core) ==
               0); /* DETECT: the playlist's */

    const struct ps5_library_system *gb = system_of(&library, "gb");
    snprintf(path, sizeof(path), "%s/mgba_libretro.so", cores);
    assert(gb && strcmp(gb->core, path) == 0);
    const struct ps5_library_system *cps = system_of(&library, "cps");
    snprintf(path, sizeof(path), "%s/fbneo_libretro.so", cores);
    assert(cps && cps->known && strcmp(cps->core, path) == 0); /* FBNeo first, then MAME */

    const struct ps5_library_system *homebrew = system_of(&library, "homebrew");
    assert(homebrew && !homebrew->known && strcmp(homebrew->name, "Homebrew") == 0 &&
           !homebrew->core[0]);

    /* What RetroArch remembers of play, with each game. */
    {
        const struct ps5_library_game *mario = &library.games[snes->first_game + 1];
        assert(mario->favorite && mario->history == 3 && mario->play_count == 3 &&
               mario->play_seconds == 3907 &&
               strcmp(mario->last_played, "2026-10-01 10:00:00") == 0);
        const struct ps5_library_game *dkc2 = &library.games[snes->first_game];
        assert(!dkc2->favorite && !dkc2->history && !dkc2->last_played[0] && !dkc2->play_count);
        /* No runtime log: a time from the place in the history, a minute a place. */
        struct stat written;
        assert(stat(history, &written) == 0);
        const struct ps5_library_game *okami = &library.games[ps2->first_game + 1];
        const struct ps5_library_game *crash = NULL;
        for (size_t i = 0; i < library.game_count; i++)
            if (strcmp(library.games[i].path, "/app0/content/PS1/Crash (USA).cue") == 0)
                crash = &library.games[i];
        assert(strcmp(okami->label, "Okami") == 0 && okami->history == 1 && crash &&
               crash->history == 2);
        char expected[20];
        struct tm local;
        time_t when = written.st_mtime;
        strftime(expected, sizeof(expected), "%Y-%m-%d %H:%M:%S", localtime_r(&when, &local));
        assert(strcmp(okami->last_played, expected) == 0);
        when -= 60;
        strftime(expected, sizeof(expected), "%Y-%m-%d %H:%M:%S", localtime_r(&when, &local));
        assert(strcmp(crash->last_played, expected) == 0);
    }

    /* Game mode's commands, parsed back. */
    char command[4096];
    struct ps5_game parsed;
    const struct ps5_library_game *mario = &library.games[snes->first_game + 1];
    assert(ps5_library_command(&library, mario, command, sizeof(command)) == 0);
    assert(ps5_game_parse_command(command, &parsed) == 0);
    assert(strcmp(parsed.content, mario->path) == 0 && strcmp(parsed.core, snes->core) == 0);
    const struct ps5_library_game *demo = &library.games[homebrew->first_game];
    assert(ps5_library_command(&library, demo, command, sizeof(command)) == 0);
    assert(ps5_game_parse_command(command, &parsed) == 0 && !parsed.core[0] &&
           strcmp(parsed.content, demo->path) == 0);
    assert(ps5_library_command(&library, mario, command, 20) == -1);

    ps5_library_free(&library);
    assert(ps5_library_load(&library, "/nonexistent", "/nonexistent", "/nonexistent") == 0 &&
           library.system_count == 0 && library.game_count == 0);
    ps5_library_free(&library);
    puts("ps5_library: platforms, playlists, systems, cores, folders, labels, favourites, history, "
         "runtime logs and game mode commands PASS");
    return 0;
}
