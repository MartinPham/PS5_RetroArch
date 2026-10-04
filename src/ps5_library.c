/* PS5 RetroArch - the game library: what every frontend shows, read from RetroArch.
 *
 * Copyright (C) 2026 Mihawk
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * src/ps5_library.h says what this is and who uses it.
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "ps5_library.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* --- the JSON RetroArch writes its playlists in ----------------------------------- */

struct json
{
    const char *at, *end;
};

static void json_space(struct json *json)
{
    while (json->at < json->end && isspace((unsigned char)*json->at))
        json->at++;
}

static void put_utf8(char *out, size_t size, size_t *length, unsigned code)
{
    unsigned char bytes[4];
    size_t count;
    if (code < 0x80)
        bytes[0] = (unsigned char)code, count = 1;
    else if (code < 0x800)
        bytes[0] = (unsigned char)(0xc0 | code >> 6),
        bytes[1] = (unsigned char)(0x80 | (code & 0x3f)), count = 2;
    else if (code < 0x10000)
        bytes[0] = (unsigned char)(0xe0 | code >> 12),
        bytes[1] = (unsigned char)(0x80 | ((code >> 6) & 0x3f)),
        bytes[2] = (unsigned char)(0x80 | (code & 0x3f)), count = 3;
    else
        bytes[0] = (unsigned char)(0xf0 | code >> 18),
        bytes[1] = (unsigned char)(0x80 | ((code >> 12) & 0x3f)),
        bytes[2] = (unsigned char)(0x80 | ((code >> 6) & 0x3f)),
        bytes[3] = (unsigned char)(0x80 | (code & 0x3f)), count = 4;
    for (size_t i = 0; i < count; i++)
        if (*length + 1 < size)
            out[(*length)++] = (char)bytes[i];
}

static int hex4(const char *at, unsigned *value)
{
    *value = 0;
    for (int i = 0; i < 4; i++)
    {
        const char c = at[i];
        const int digit = c >= '0' && c <= '9'   ? c - '0'
                          : c >= 'a' && c <= 'f' ? c - 'a' + 10
                          : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                                 : -1;
        if (digit < 0)
            return -1;
        *value = *value << 4 | (unsigned)digit;
    }
    return 0;
}

/* A JSON string, unescaped into out (truncated to size); 0, or -1 when malformed. */
static int json_string(struct json *json, char *out, size_t size)
{
    size_t length = 0;
    if (json->at >= json->end || *json->at != '"')
        return -1;
    json->at++;
    while (json->at < json->end && *json->at != '"')
    {
        char c = *json->at++;
        if (c == '\\')
        {
            if (json->at >= json->end)
                return -1;
            c = *json->at++;
            unsigned code;
            switch (c)
            {
            case 'b':
                c = '\b';
                break;
            case 'f':
                c = '\f';
                break;
            case 'n':
                c = '\n';
                break;
            case 'r':
                c = '\r';
                break;
            case 't':
                c = '\t';
                break;
            case 'u':
                if (json->end - json->at < 4 || hex4(json->at, &code) != 0)
                    return -1;
                json->at += 4;
                if (code >= 0xd800 && code < 0xdc00 && json->end - json->at >= 6 &&
                    json->at[0] == '\\' && json->at[1] == 'u')
                {
                    unsigned low;
                    if (hex4(json->at + 2, &low) == 0 && low >= 0xdc00 && low < 0xe000)
                    {
                        code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00);
                        json->at += 6;
                    }
                }
                put_utf8(out, size, &length, code);
                continue;
            default:
                break; /* '"', '\\' and '/' stand for themselves */
            }
        }
        if (length + 1 < size)
            out[length++] = c;
    }
    if (json->at >= json->end)
        return -1;
    json->at++;
    if (size)
        out[length] = '\0';
    return 0;
}

/* Skips one JSON value of any kind; 0, or -1 when malformed. */
static int json_skip(struct json *json, int depth)
{
    json_space(json);
    if (json->at >= json->end || depth > 64)
        return -1;
    const char c = *json->at;
    if (c == '"')
    {
        char ignored[1];
        return json_string(json, ignored, 0);
    }
    if (c == '{' || c == '[')
    {
        const char close = c == '{' ? '}' : ']';
        json->at++;
        json_space(json);
        if (json->at < json->end && *json->at == close)
        {
            json->at++;
            return 0;
        }
        for (;;)
        {
            if (c == '{')
            {
                char ignored[1];
                json_space(json);
                if (json_string(json, ignored, 0) != 0)
                    return -1;
                json_space(json);
                if (json->at >= json->end || *json->at++ != ':')
                    return -1;
            }
            if (json_skip(json, depth + 1) != 0)
                return -1;
            json_space(json);
            if (json->at >= json->end)
                return -1;
            if (*json->at == ',')
            {
                json->at++;
                continue;
            }
            if (*json->at++ != close)
                return -1;
            return 0;
        }
    }
    while (json->at < json->end && !strchr(",]} \t\r\n", *json->at))
        json->at++; /* a number, true, false or null */
    return 0;
}

/* A JSON object's string member values the reader keeps; the rest is skipped. */
struct member
{
    const char *key;
    char *value;
    size_t size;
};

/* Reads one object, keeping the members named in wanted; 0, or -1 when malformed. When
 * array_key is named, the position of that member's value is kept in *array and the
 * value skipped. */
static int json_object(struct json *json, struct member *wanted, size_t count,
                       const char *array_key, struct json *array)
{
    json_space(json);
    if (json->at >= json->end || *json->at++ != '{')
        return -1;
    for (size_t i = 0; i < count; i++)
        if (wanted[i].size)
            wanted[i].value[0] = '\0';
    for (;;)
    {
        char key[64];
        json_space(json);
        if (json->at < json->end && *json->at == '}')
        {
            json->at++;
            return 0;
        }
        if (json_string(json, key, sizeof(key)) != 0)
            return -1;
        json_space(json);
        if (json->at >= json->end || *json->at++ != ':')
            return -1;
        json_space(json);
        int kept = 0;
        for (size_t i = 0; i < count && !kept; i++)
            if (strcmp(key, wanted[i].key) == 0 && json->at < json->end && *json->at == '"')
            {
                if (json_string(json, wanted[i].value, wanted[i].size) != 0)
                    return -1;
                kept = 1;
            }
        if (!kept)
        {
            if (array_key && array && strcmp(key, array_key) == 0)
                *array = *json;
            if (json_skip(json, 1) != 0)
                return -1;
        }
        json_space(json);
        if (json->at < json->end && *json->at == ',')
            json->at++;
    }
}

int ps5_playlist_read(const char *file, char *default_core, size_t default_core_size,
                      void (*each)(void *context, const struct ps5_playlist_entry *entry),
                      void *context)
{
    if (default_core_size)
        default_core[0] = '\0';
    FILE *input = fopen(file, "rb");
    if (!input)
        return -1;
    char *text = NULL;
    long length = 0;
    if (fseek(input, 0, SEEK_END) == 0 && (length = ftell(input)) > 0 && length < (64L << 20) &&
        fseek(input, 0, SEEK_SET) == 0 && (text = (char *)malloc((size_t)length)) != NULL &&
        fread(text, 1, (size_t)length, input) != (size_t)length)
    {
        free(text);
        text = NULL;
    }
    fclose(input);
    if (!text)
        return -1;
    struct json json = {text, text + length}, items = {NULL, NULL};
    char ignored[1];
    struct member header[] = {{"default_core_path", default_core_size ? default_core : ignored,
                               default_core_size ? default_core_size : sizeof(ignored)}};
    int count = -1;
    if (json_object(&json, header, 1, "items", &items) == 0)
    {
        count = 0;
        if (items.at)
        {
            json_space(&items);
            if (items.at < items.end && *items.at == '[')
            {
                items.at++;
                for (;;)
                {
                    json_space(&items);
                    if (items.at >= items.end || *items.at == ']')
                        break;
                    static char path[PS5_LIBRARY_PATH_MAX], label[512], core[PS5_LIBRARY_PATH_MAX],
                        crc[64], database[256];
                    struct member entry[] = {{"path", path, sizeof(path)},
                                             {"label", label, sizeof(label)},
                                             {"core_path", core, sizeof(core)},
                                             {"crc32", crc, sizeof(crc)},
                                             {"db_name", database, sizeof(database)}};
                    if (*items.at != '{')
                    {
                        if (json_skip(&items, 1) != 0)
                            break;
                    }
                    else if (json_object(&items, entry, 5, NULL, NULL) != 0)
                        break;
                    else if (path[0])
                    {
                        const struct ps5_playlist_entry found = {path, label, core, crc, database};
                        if (each)
                            each(context, &found);
                        count++;
                    }
                    json_space(&items);
                    if (items.at < items.end && *items.at == ',')
                        items.at++;
                }
            }
        }
    }
    free(text);
    return count;
}

/* --- platforms -------------------------------------------------------------------- */

/* A platform: its id (the short name frontends share: ES-DE's, RetroPie's,
 * Batocera's), its RetroArch database, the databases whose cores run it (in order of
 * preference, for a platform no single database names), the names it goes by exactly,
 * and the words a longer name gives it away by. Ordered most specific first: a name is
 * matched exactly against every platform before any word is looked for. */
struct platform
{
    const char *id, *database, *core_databases, *names, *words;
};

static const struct platform platforms[] = {
    {"gba", "Nintendo - Game Boy Advance", NULL, "gba", "gameboyadvance"},
    {"gbc", "Nintendo - Game Boy Color", NULL, "gbc", "gameboycolor|gameboycolour"},
    {"gb", "Nintendo - Game Boy", NULL, "gb", "gameboy"},
    {"n3ds", "Nintendo - Nintendo 3DS", NULL, "3ds|n3ds", "nintendo3ds"},
    {"nds", "Nintendo - Nintendo DS", NULL, "nds|ds", "nintendods"},
    {"n64", "Nintendo - Nintendo 64", NULL, "n64", "nintendo64"},
    {"snes", "Nintendo - Super Nintendo Entertainment System", NULL, "snes|sfc",
     "supernintendo|superfamicom"},
    {"fds", "Nintendo - Family Computer Disk System", NULL, "fds", "famicomdisk"},
    {"nes", "Nintendo - Nintendo Entertainment System", NULL, "nes|famicom",
     "nintendoentertainmentsystem"},
    {"gc", "Nintendo - GameCube", NULL, "gc|ngc", "gamecube"},
    {"wii", "Nintendo - Wii", NULL, "wii", "nintendowii"},
    {"ps2", "Sony - PlayStation 2", NULL, "ps2", "playstation2"},
    {"psp", "Sony - PlayStation Portable", NULL, "psp", "playstationportable"},
    {"psx", "Sony - PlayStation", NULL, "psx|ps1|psone|playstation|sonyplaystation",
     "playstation1"},
    {"segacd", "Sega - Mega-CD - Sega CD", NULL, "segacd|megacd", "segacd|megacd"},
    {"sega32x", "Sega - 32X", NULL, "32x|sega32x", "sega32x"},
    {"genesis", "Sega - Mega Drive - Genesis", NULL, "genesis|megadrive|md", "megadrive|genesis"},
    {"mastersystem", "Sega - Master System - Mark III", NULL, "sms|mastersystem", "mastersystem"},
    {"gamegear", "Sega - Game Gear", NULL, "gg|gamegear", "gamegear"},
    {"sg-1000", "Sega - SG-1000", NULL, "sg1000", "sg1000"},
    {"saturn", "Sega - Saturn", NULL, "saturn", "segasaturn|saturn"},
    {"dreamcast", "Sega - Dreamcast", NULL, "dreamcast|dc", "dreamcast"},
    {"c64", "Commodore - 64", NULL, "c64|commodore64", "commodore64"},
    {"amiga", "Commodore - Amiga", NULL, "amiga", "amiga"},
    {"atari2600", "Atari - 2600", NULL, "atari2600|2600", "atari2600"},
    {"atari7800", "Atari - 7800", NULL, "atari7800|7800", "atari7800"},
    {"atarilynx", "Atari - Lynx", NULL, "lynx|atarilynx", "atarilynx"},
    {"pcengine", "NEC - PC Engine - TurboGrafx 16", NULL, "pce|pcengine|tg16|turbografx16",
     "pcengine|turbografx"},
    {"ngp", "SNK - Neo Geo Pocket", NULL, "ngp|neogeopocket", "neogeopocket"},
    {"wonderswan", "Bandai - WonderSwan", NULL, "ws|wonderswan", "wonderswan"},
    {"msx", "Microsoft - MSX", NULL, "msx", "msx"},
    {"neogeo", "SNK - Neo Geo", "SNK - Neo Geo|FBNeo - Arcade Games|MAME", "neogeo|neo", "neogeo"},
    {"cps", NULL, "FBNeo - Arcade Games|MAME", "cps|cps1|cps2|cps3", "cps"},
    {"fbneo", "FBNeo - Arcade Games", NULL, "fbneo|fba|finalburn", "fbneo|finalburn"},
    {"mame", "MAME", NULL, "mame", "mame"},
    {"arcade", NULL, "MAME|FBNeo - Arcade Games", "arcade", "arcade"},
};

static void normalize(const char *name, char *out, size_t size)
{
    size_t length = 0;
    for (; *name && length + 1 < size; name++)
        if (isalnum((unsigned char)*name))
            out[length++] = (char)tolower((unsigned char)*name);
    out[length] = '\0';
}

/* Whether a '|'-separated list holds item (exactly). */
static int list_has(const char *list, const char *item)
{
    const size_t length = strlen(item);
    for (const char *at = list; at && *at;)
    {
        const char *bar = strchr(at, '|');
        const size_t part = bar ? (size_t)(bar - at) : strlen(at);
        if (part == length && strncmp(at, item, length) == 0)
            return 1;
        at = bar ? bar + 1 : NULL;
    }
    return 0;
}

const char *ps5_library_platform(const char *name)
{
    char base[256], key[256];
    snprintf(base, sizeof(base), "%s", name ? name : "");
    const size_t length = strlen(base);
    if (length > 4 && strcmp(base + length - 4, ".lpl") == 0)
        base[length - 4] = '\0';
    normalize(base, key, sizeof(key));
    if (!key[0])
        return NULL;
    const size_t count = sizeof(platforms) / sizeof(platforms[0]);
    for (size_t i = 0; i < count; i++)
    {
        char database[256];
        normalize(platforms[i].database ? platforms[i].database : "", database, sizeof(database));
        if ((database[0] && strcmp(database, key) == 0) || list_has(platforms[i].names, key))
            return platforms[i].id;
    }
    for (size_t i = 0; i < count; i++)
        for (const char *word = platforms[i].words; word && *word;)
        {
            const char *bar = strchr(word, '|');
            char part[64];
            snprintf(part, sizeof(part), "%.*s", (int)(bar ? (size_t)(bar - word) : strlen(word)),
                     word);
            if (strstr(key, part))
                return platforms[i].id;
            word = bar ? bar + 1 : NULL;
        }
    return NULL;
}

static const struct platform *platform_by_id(const char *id)
{
    for (size_t i = 0; id && i < sizeof(platforms) / sizeof(platforms[0]); i++)
        if (strcmp(platforms[i].id, id) == 0)
            return &platforms[i];
    return NULL;
}

const char *ps5_library_platform_database(const char *id)
{
    const struct platform *platform = platform_by_id(id);
    return platform ? platform->database : NULL;
}

/* --- core info -------------------------------------------------------------------- */

/* A core info value: the text between the quotes of `key = "value"`. */
static int info_value(const char *line, const char *key, char *out, size_t size)
{
    const size_t length = strlen(key);
    while (*line == ' ' || *line == '\t')
        line++;
    if (strncmp(line, key, length) != 0)
        return 0;
    line += length;
    while (*line == ' ' || *line == '\t')
        line++;
    if (*line++ != '=')
        return 0;
    while (*line == ' ' || *line == '\t')
        line++;
    if (*line++ != '"')
        return 0;
    const char *close = strchr(line, '"');
    if (!close)
        return 0;
    snprintf(out, size, "%.*s", (int)(close - line), line);
    return 1;
}

static int is_file(const char *path)
{
    struct stat status;
    return stat(path, &status) == 0 && S_ISREG(status.st_mode);
}

static int compare_names(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* The names in a folder ending with suffix, sorted; NULL-terminated, or NULL. */
static char **folder_names(const char *folder, const char *suffix)
{
    DIR *directory = opendir(folder);
    if (!directory)
        return NULL;
    size_t count = 0, capacity = 16;
    char **names = (char **)malloc(capacity * sizeof(char *));
    const size_t suffix_length = strlen(suffix);
    for (struct dirent *entry; names && (entry = readdir(directory)) != NULL;)
    {
        const size_t length = strlen(entry->d_name);
        if (length <= suffix_length || strcmp(entry->d_name + length - suffix_length, suffix) != 0)
            continue;
        if (count + 1 >= capacity)
        {
            char **grown = (char **)realloc(names, (capacity *= 2) * sizeof(char *));
            if (!grown)
            {
                for (size_t i = 0; i < count; i++)
                    free(names[i]);
                free(names);
                names = NULL;
                break;
            }
            names = grown;
        }
        if (!(names[count] = (char *)malloc(length + 1)))
            continue;
        memcpy(names[count++], entry->d_name, length + 1);
    }
    closedir(directory);
    if (!names)
        return NULL;
    names[count] = NULL;
    qsort(names, count, sizeof(char *), compare_names);
    return names;
}

static void free_names(char **names)
{
    for (size_t i = 0; names && names[i]; i++)
        free(names[i]);
    free(names);
}

/* --- the library ------------------------------------------------------------------ */

struct loading
{
    struct ps5_library *library;
    const char *playlist;     /* the file name */
    const char *default_core; /* the playlist's */
    size_t game_capacity;
    int failed;
};

static const struct ps5_library_core *core_by_path(const struct ps5_library *library,
                                                   const char *path)
{
    for (size_t i = 0; path && path[0] && i < library->core_count; i++)
        if (strcmp(library->cores[i].path, path) == 0)
            return &library->cores[i];
    return NULL;
}

/* The core a playlist names for an entry, when it is one the title carries. */
static const char *entry_core(const struct ps5_library *library, const char *core,
                              const char *default_core)
{
    if (core_by_path(library, core))
        return core;
    if ((!core || !core[0] || strcmp(core, "DETECT") == 0) && core_by_path(library, default_core))
        return default_core;
    return "";
}

static void parent_folder_name(const char *path, char *out, size_t size)
{
    const char *slash = strrchr(path, '/');
    if (!slash || slash == path)
    {
        snprintf(out, size, "%s", "");
        return;
    }
    const char *start = slash - 1;
    while (start > path && *start != '/')
        start--;
    if (*start == '/')
        start++;
    snprintf(out, size, "%.*s", (int)(slash - start), start);
}

static void strip_lpl(const char *name, char *out, size_t size)
{
    snprintf(out, size, "%s", name);
    const size_t length = strlen(out);
    if (length > 4 && strcmp(out + length - 4, ".lpl") == 0)
        out[length - 4] = '\0';
}

/* The system of an entry: a platform id and its name (*known 1), or a key of its own. */
static void entry_system(const struct ps5_library *library, const struct ps5_playlist_entry *entry,
                         const char *playlist, const char *core, char *id, size_t id_size,
                         char *name, size_t name_size, int *known)
{
    char database[256], list[256], folder[256];
    strip_lpl(entry->db_name ? entry->db_name : "", database, sizeof(database));
    strip_lpl(playlist, list, sizeof(list));
    parent_folder_name(entry->path, folder, sizeof(folder));
    const char *candidates[] = {database, list, folder};
    for (size_t i = 0; i < 3; i++)
    {
        const char *platform = candidates[i][0] ? ps5_library_platform(candidates[i]) : NULL;
        if (platform)
        {
            const char *canonical = ps5_library_platform_database(platform);
            snprintf(id, id_size, "%s", platform);
            snprintf(name, name_size, "%s", canonical ? canonical : candidates[i]);
            *known = 1;
            return;
        }
    }
    const struct ps5_library_core *info = core_by_path(library, core);
    if (info && info->databases[0])
    {
        char first[256];
        const char *bar = strchr(info->databases, '|');
        snprintf(first, sizeof(first), "%.*s",
                 (int)(bar ? (size_t)(bar - info->databases) : strlen(info->databases)),
                 info->databases);
        const char *platform = ps5_library_platform(first);
        if (platform)
        {
            snprintf(id, id_size, "%s", platform);
            snprintf(name, name_size, "%s", first);
            *known = 1;
            return;
        }
    }
    /* Its own system: named after the playlist, unless that is the mixed one every game
     * went into (its entries' databases name only it), then after the folder. */
    const int mixed = strcmp(database, list) == 0 && folder[0];
    snprintf(name, name_size, "%s", mixed ? folder : list);
    normalize(name, id, id_size);
    *known = 0;
}

static void add_entry(void *context, const struct ps5_playlist_entry *entry)
{
    struct loading *loading = (struct loading *)context;
    struct ps5_library *library = loading->library;
    if (loading->failed)
        return;
    for (size_t i = 0; i < library->game_count; i++)
        if (strcmp(library->games[i].path, entry->path) == 0)
            return; /* a game in two playlists is one game */
    if (library->game_count == loading->game_capacity)
    {
        const size_t capacity = loading->game_capacity ? loading->game_capacity * 2 : 64;
        struct ps5_library_game *grown = (struct ps5_library_game *)realloc(
            library->games, capacity * sizeof(struct ps5_library_game));
        if (!grown)
        {
            loading->failed = 1;
            return;
        }
        library->games = grown;
        loading->game_capacity = capacity;
    }
    struct ps5_library_game *game = &library->games[library->game_count];
    memset(game, 0, sizeof(*game));
    snprintf(game->path, sizeof(game->path), "%s", entry->path);
    if (entry->label && entry->label[0])
        snprintf(game->label, sizeof(game->label), "%s", entry->label);
    else
    {
        const char *slash = strrchr(entry->path, '/');
        const char *file = slash ? slash + 1 : entry->path;
        const char *dot = strrchr(file, '.');
        snprintf(game->label, sizeof(game->label), "%.*s",
                 (int)(dot ? (size_t)(dot - file) : strlen(file)), file);
    }
    snprintf(game->core, sizeof(game->core), "%s",
             entry_core(library, entry->core_path, loading->default_core));
    snprintf(game->crc32, sizeof(game->crc32), "%s", entry->crc32 ? entry->crc32 : "");
    snprintf(game->playlist, sizeof(game->playlist), "%s", loading->playlist);

    char id[64], name[256];
    int known = 0;
    entry_system(library, entry, loading->playlist, game->core, id, sizeof(id), name, sizeof(name),
                 &known);
    size_t system = 0;
    while (system < library->system_count && strcmp(library->systems[system].id, id) != 0)
        system++;
    if (system == library->system_count)
    {
        struct ps5_library_system *grown = (struct ps5_library_system *)realloc(
            library->systems, (library->system_count + 1) * sizeof(struct ps5_library_system));
        if (!grown)
        {
            loading->failed = 1;
            return;
        }
        library->systems = grown;
        memset(&library->systems[system], 0, sizeof(library->systems[system]));
        snprintf(library->systems[system].id, sizeof(library->systems[system].id), "%s", id);
        snprintf(library->systems[system].name, sizeof(library->systems[system].name), "%s", name);
        library->systems[system].known = known;
        library->system_count++;
    }
    game->system = system;
    library->game_count++;
}

static int is_builtin_playlist(const char *name)
{
    static const char *const builtin[] = {"content_history.lpl",       "content_favorites.lpl",
                                          "content_music_history.lpl", "content_video_history.lpl",
                                          "content_image_history.lpl", NULL};
    for (size_t i = 0; builtin[i]; i++)
        if (strcmp(name, builtin[i]) == 0)
            return 1;
    return 0;
}

static void load_cores(struct ps5_library *library, const char *info, const char *cores)
{
    char **names = folder_names(info, ".info");
    size_t count = 0;
    while (names && names[count])
        count++;
    if (count && (library->cores =
                      (struct ps5_library_core *)calloc(count, sizeof(struct ps5_library_core))))
        for (size_t i = 0; i < count; i++)
        {
            struct ps5_library_core *core = &library->cores[library->core_count];
            char path[PS5_LIBRARY_PATH_MAX + 512];
            snprintf(path, sizeof(path), "%s/%.*s.so", cores, (int)(strlen(names[i]) - 5),
                     names[i]);
            if (!is_file(path) || strlen(path) >= sizeof(core->path))
                continue;
            memcpy(core->path, path, strlen(path) + 1);
            snprintf(path, sizeof(path), "%s/%s", info, names[i]);
            FILE *file = fopen(path, "r");
            if (!file)
                continue;
            char line[2048];
            while (fgets(line, sizeof(line), file))
            {
                info_value(line, "display_name", core->name, sizeof(core->name));
                info_value(line, "database", core->databases, sizeof(core->databases));
                info_value(line, "supported_extensions", core->extensions,
                           sizeof(core->extensions));
            }
            fclose(file);
            library->core_count++;
        }
    free_names(names);
}

/* Adds each of a '|'-separated list of extensions to set, lower case, once. */
static void add_extensions(char *set, size_t size, const char *list)
{
    for (const char *at = list; at && *at;)
    {
        const char *bar = strchr(at, '|');
        char extension[32];
        size_t length = bar ? (size_t)(bar - at) : strlen(at);
        if (length && length < sizeof(extension))
        {
            for (size_t i = 0; i < length; i++)
                extension[i] = (char)tolower((unsigned char)at[i]);
            extension[length] = '\0';
            if (!list_has(set, extension) && strlen(set) + length + 2 < size)
            {
                if (set[0])
                    strcat(set, "|");
                strcat(set, extension);
            }
        }
        at = bar ? bar + 1 : NULL;
    }
}

static int compare_games(const void *a, const void *b)
{
    const struct ps5_library_game *x = (const struct ps5_library_game *)a,
                                  *y = (const struct ps5_library_game *)b;
    if (x->system != y->system)
        return x->system < y->system ? -1 : 1;
    const int label = strcmp(x->label, y->label);
    return label ? label : strcmp(x->path, y->path);
}

static void finish_systems(struct ps5_library *library)
{
    for (size_t s = 0; s < library->system_count; s++)
    {
        struct ps5_library_system *system = &library->systems[s];
        /* Its core: the one its entries name most. */
        size_t best = 0;
        for (size_t i = 0; i < library->game_count; i++)
        {
            const struct ps5_library_game *game = &library->games[i];
            if (game->system != s || !game->core[0])
                continue;
            size_t votes = 0;
            for (size_t j = 0; j < library->game_count; j++)
                votes += library->games[j].system == s &&
                         strcmp(library->games[j].core, game->core) == 0;
            if (votes > best)
            {
                best = votes;
                snprintf(system->core, sizeof(system->core), "%s", game->core);
            }
        }
        /* Else the first core, in path order, whose info names the platform's database
         * (or one of the databases that run it). */
        const struct platform *platform = platform_by_id(system->id);
        if (!system->core[0] && platform)
        {
            char databases[512];
            snprintf(databases, sizeof(databases), "%s",
                     platform->core_databases ? platform->core_databases
                     : platform->database     ? platform->database
                                              : "");
            for (const char *at = databases; at && *at && !system->core[0];)
            {
                const char *bar = strchr(at, '|');
                char database[256];
                snprintf(database, sizeof(database), "%.*s",
                         (int)(bar ? (size_t)(bar - at) : strlen(at)), at);
                for (size_t c = 0; c < library->core_count && !system->core[0]; c++)
                    if (list_has(library->cores[c].databases, database))
                        snprintf(system->core, sizeof(system->core), "%s", library->cores[c].path);
                at = bar ? bar + 1 : NULL;
            }
        }
        /* Its folder and extensions. */
        int first = 1;
        for (size_t i = 0; i < library->game_count; i++)
        {
            const struct ps5_library_game *game = &library->games[i];
            if (game->system != s)
                continue;
            const char *slash = strrchr(game->path, '/');
            const char *dot = strrchr(game->path, '.');
            if (dot && (!slash || dot > slash))
                add_extensions(system->extensions, sizeof(system->extensions), dot + 1);
            const size_t folder_length = slash ? (size_t)(slash - game->path) : 0;
            if (first)
            {
                snprintf(system->folder, sizeof(system->folder), "%.*s", (int)folder_length,
                         game->path);
                first = 0;
                continue;
            }
            size_t common = 0;
            while (common < folder_length && system->folder[common] &&
                   system->folder[common] == game->path[common])
                common++;
            /* Back to a whole folder: the common part ends at a '/' of both. */
            if (!(system->folder[common] == '\0' &&
                  (common == folder_length || game->path[common] == '/')))
                while (common > 0 && system->folder[common] != '/')
                    common--;
            system->folder[common] = '\0';
        }
        if (!system->folder[0])
            snprintf(system->folder, sizeof(system->folder), "%s", "/");
        const struct ps5_library_core *core = core_by_path(library, system->core);
        if (core)
            add_extensions(system->extensions, sizeof(system->extensions), core->extensions);
    }
    /* Games by system, then label; each system's range. */
    qsort(library->games, library->game_count, sizeof(struct ps5_library_game), compare_games);
    for (size_t i = 0; i < library->game_count; i++)
    {
        struct ps5_library_system *system = &library->systems[library->games[i].system];
        if (system->game_count++ == 0)
            system->first_game = i;
    }
}

int ps5_library_load(struct ps5_library *library, const char *playlists, const char *info,
                     const char *cores)
{
    memset(library, 0, sizeof(*library));
    load_cores(library, info, cores);
    char **names = folder_names(playlists, ".lpl");
    struct loading loading = {library, NULL, NULL, 0, 0};
    for (size_t i = 0; names && names[i] && !loading.failed; i++)
    {
        if (is_builtin_playlist(names[i]))
            continue;
        char path[PS5_LIBRARY_PATH_MAX + 512], default_core[PS5_LIBRARY_PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", playlists, names[i]);
        loading.playlist = names[i];
        loading.default_core = default_core;
        ps5_playlist_read(path, default_core, sizeof(default_core), add_entry, &loading);
    }
    free_names(names);
    if (loading.failed)
        return -1;
    finish_systems(library);
    return 0;
}

void ps5_library_free(struct ps5_library *library)
{
    free(library->systems);
    free(library->games);
    free(library->cores);
    memset(library, 0, sizeof(*library));
}

/* Appends a shell word, single-quoted: ' becomes '\''. 0, or -1 when out is full. */
static int append_quoted(char *out, size_t size, size_t *length, const char *word)
{
    const char *parts[] = {" '", word, "'"};
    for (size_t p = 0; p < 3; p++)
        for (const char *at = parts[p]; *at; at++)
        {
            const char *piece = p == 1 && *at == '\'' ? "'\\''" : NULL;
            const size_t piece_length = piece ? 4 : 1;
            if (*length + piece_length + 1 > size)
                return -1;
            memcpy(out + *length, piece ? piece : at, piece_length);
            *length += piece_length;
        }
    out[*length] = '\0';
    return 0;
}

int ps5_library_command(const struct ps5_library *library, const struct ps5_library_game *game,
                        char *out, size_t size)
{
    static const char program[] = "/app0/eboot.bin";
    if (size < sizeof(program))
        return -1;
    memcpy(out, program, sizeof(program));
    size_t length = sizeof(program) - 1;
    const char *core =
        game->system < library->system_count ? library->systems[game->system].core : "";
    if (core[0])
    {
        if (length + 4 > size)
            return -1;
        memcpy(out + length, " -L", 4);
        length += 3;
        if (append_quoted(out, size, &length, core) != 0)
            return -1;
    }
    return append_quoted(out, size, &length, game->path);
}
