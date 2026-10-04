/* PS5 RetroArch - game mode: how every frontend starts a game in RetroArch.
 *
 * Copyright (C) 2026 Mihawk
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * src/ps5_game.h says what this is and who uses it.
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "ps5_game.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

int sceSystemServiceLoadExec(const char *path, const char *const *argv);

static void set_error(struct ps5_game *game, const char *text)
{
    snprintf(game->error, sizeof(game->error), "%s", text);
}

static int is_file(const char *path)
{
    struct stat status;
    return path && path[0] && stat(path, &status) == 0 && S_ISREG(status.st_mode);
}

static int has_line_break(const char *text)
{
    return strchr(text, '\n') != NULL || strchr(text, '\r') != NULL;
}

int ps5_game_write(const char *path, const struct ps5_game *game, int result)
{
    char temporary[PS5_GAME_PATH_MAX + 8];
    snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    FILE *file = fopen(temporary, "w");
    if (!file)
        return -1;
    fprintf(file, "ps5-game 1\nkind=%s\ncore=%s\ncontent=%s\nfrontend=%s\nstate=%s\n",
            result ? "result" : "request", game->core, game->content, game->frontend, game->state);
    if (result)
        fprintf(file, "status=%d\nseconds=%ld\nerror=%s\n", game->status, game->seconds,
                game->error);
    const int written = ferror(file) == 0;
    if (fclose(file) != 0 || !written)
    {
        remove(temporary);
        return -1;
    }
    chmod(temporary, 0666);
    if (rename(temporary, path) != 0)
    {
        remove(temporary);
        return -1;
    }
    return 0;
}

static void copy_value(char *out, size_t size, const char *value)
{
    snprintf(out, size, "%s", value);
}

int ps5_game_read(const char *path, struct ps5_game *game, int *result)
{
    memset(game, 0, sizeof(*game));
    FILE *file = fopen(path, "r");
    if (!file)
        return -1;
    char line[PS5_GAME_STATE_MAX + 32];
    int valid = fgets(line, sizeof(line), file) && strcmp(line, "ps5-game 1\n") == 0;
    int kind = -1;
    while (valid && fgets(line, sizeof(line), file))
    {
        const size_t length = strlen(line);
        if (length == 0 || line[length - 1] != '\n')
        {
            valid = 0; /* a line longer than any value may be */
            break;
        }
        line[length - 1] = '\0';
        char *value = strchr(line, '=');
        if (!value)
            continue;
        *value++ = '\0';
        if (strcmp(line, "kind") == 0)
            kind = strcmp(value, "result") == 0 ? 1 : strcmp(value, "request") == 0 ? 0 : -1;
        else if (strcmp(line, "core") == 0)
            copy_value(game->core, sizeof(game->core), value);
        else if (strcmp(line, "content") == 0)
            copy_value(game->content, sizeof(game->content), value);
        else if (strcmp(line, "frontend") == 0)
            copy_value(game->frontend, sizeof(game->frontend), value);
        else if (strcmp(line, "state") == 0)
            copy_value(game->state, sizeof(game->state), value);
        else if (strcmp(line, "status") == 0)
            game->status = atoi(value);
        else if (strcmp(line, "seconds") == 0)
            game->seconds = atol(value);
        else if (strcmp(line, "error") == 0)
            copy_value(game->error, sizeof(game->error), value);
    }
    fclose(file);
    if (!valid || kind < 0)
        return -1;
    if (result)
        *result = kind;
    return 0;
}

/* The next shell word of a command: quotes and backslashes as a POSIX shell takes
 * them. Returns the position after it, or NULL when there is none or it is too long. */
static const char *next_word(const char *at, char *word, size_t size)
{
    while (*at && isspace((unsigned char)*at))
        at++;
    if (!*at)
        return NULL;
    size_t length = 0;
    char quote = 0;
    for (; *at && (quote || !isspace((unsigned char)*at)); at++)
    {
        char c = *at;
        if (quote == '\'')
        {
            if (c == '\'')
            {
                quote = 0;
                continue;
            }
        }
        else if (quote == '"')
        {
            if (c == '"')
            {
                quote = 0;
                continue;
            }
            if (c == '\\' && (at[1] == '"' || at[1] == '\\' || at[1] == '$' || at[1] == '`'))
                c = *++at;
        }
        else if (c == '\'' || c == '"')
        {
            quote = c;
            continue;
        }
        else if (c == '\\' && at[1])
            c = *++at;
        if (length + 1 >= size)
            return NULL;
        word[length++] = c;
    }
    if (quote)
        return NULL;
    word[length] = '\0';
    return at;
}

int ps5_game_parse_command(const char *command, struct ps5_game *game)
{
    char word[PS5_GAME_PATH_MAX];
    game->core[0] = game->content[0] = '\0';
    const char *at = command ? next_word(command, word, sizeof(word)) : NULL;
    if (!at || strcmp(word, PS5_GAME_EBOOT) != 0)
    {
        set_error(game, "the launch command does not start " PS5_GAME_EBOOT);
        return -1;
    }
    while ((at = next_word(at, word, sizeof(word))) != NULL)
    {
        if (strcmp(word, "-L") == 0)
        {
            if (!(at = next_word(at, game->core, sizeof(game->core))))
            {
                set_error(game, "the launch command names no core after -L");
                return -1;
            }
        }
        else if (word[0] == '-')
        {
            set_error(game, "the launch command has an option game mode does not take");
            return -1;
        }
        else if (game->content[0])
        {
            set_error(game, "the launch command names more than one content");
            return -1;
        }
        else
            snprintf(game->content, sizeof(game->content), "%s", word);
    }
    if (!game->content[0])
    {
        set_error(game, "the launch command names no content");
        return -1;
    }
    return 0;
}

int ps5_game_check(struct ps5_game *game)
{
    if (has_line_break(game->core) || has_line_break(game->content) ||
        has_line_break(game->frontend) || has_line_break(game->state))
    {
        set_error(game, "a value has a line break");
        return -1;
    }
    if (game->content[0] != '/' || !is_file(game->content))
    {
        set_error(game, "the content is not a file");
        return -1;
    }
    const size_t cores = strlen(PS5_GAME_CORES), suffix = strlen("_libretro.so");
    const size_t length = strlen(game->core);
    if (game->core[0] &&
        (strncmp(game->core, PS5_GAME_CORES, cores) != 0 || length <= cores + suffix ||
         strcmp(game->core + length - suffix, "_libretro.so") != 0 ||
         strchr(game->core + cores, '/') || !is_file(game->core)))
    {
        set_error(game, "the core is not a core of the title");
        return -1;
    }
    if (game->frontend[0] != '/' || !is_file(game->frontend))
    {
        set_error(game, "the frontend to come back to is not a file");
        return -1;
    }
    return 0;
}

/* --- RetroArch's playlists: a reader of the JSON they are written in ------------ */

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

/* In a playlist's items, the core of the entry whose path is content: 1 when found. */
static int playlist_items(struct json *json, const char *content, char *core, size_t size)
{
    json_space(json);
    if (json->at >= json->end || *json->at++ != '[')
        return 0;
    for (;;)
    {
        json_space(json);
        if (json->at >= json->end || *json->at == ']')
            return 0;
        if (*json->at != '{')
        {
            if (json_skip(json, 1) != 0)
                return 0;
        }
        else
        {
            char path[PS5_GAME_PATH_MAX] = "", entry_core[PS5_GAME_PATH_MAX] = "", key[32];
            json->at++;
            for (;;)
            {
                json_space(json);
                if (json->at < json->end && *json->at == '}')
                {
                    json->at++;
                    break;
                }
                if (json_string(json, key, sizeof(key)) != 0)
                    return 0;
                json_space(json);
                if (json->at >= json->end || *json->at++ != ':')
                    return 0;
                json_space(json);
                if (strcmp(key, "path") == 0 && json->at < json->end && *json->at == '"')
                {
                    if (json_string(json, path, sizeof(path)) != 0)
                        return 0;
                }
                else if (strcmp(key, "core_path") == 0 && json->at < json->end && *json->at == '"')
                {
                    if (json_string(json, entry_core, sizeof(entry_core)) != 0)
                        return 0;
                }
                else if (json_skip(json, 1) != 0)
                    return 0;
                json_space(json);
                if (json->at < json->end && *json->at == ',')
                    json->at++;
            }
            /* "DETECT" (or nothing) leaves the choice to RetroArch at launch: no core. */
            if (strcmp(path, content) == 0 && entry_core[0] && strcmp(entry_core, "DETECT") != 0)
            {
                snprintf(core, size, "%s", entry_core);
                return 1;
            }
        }
        json_space(json);
        if (json->at < json->end && *json->at == ',')
            json->at++;
    }
}

static int playlist_core(const char *text, size_t length, const char *content, char *core,
                         size_t size)
{
    struct json json = {text, text + length};
    json_space(&json);
    if (json.at >= json.end || *json.at++ != '{')
        return 0;
    for (;;)
    {
        char key[32];
        json_space(&json);
        if (json.at >= json.end || *json.at == '}' || json_string(&json, key, sizeof(key)) != 0)
            return 0;
        json_space(&json);
        if (json.at >= json.end || *json.at++ != ':')
            return 0;
        if (strcmp(key, "items") == 0)
            return playlist_items(&json, content, core, size);
        if (json_skip(&json, 1) != 0)
            return 0;
        json_space(&json);
        if (json.at < json.end && *json.at == ',')
            json.at++;
    }
}

int ps5_game_playlist_core(const char *playlists, const char *content, char *core, size_t size)
{
    DIR *folder = opendir(playlists);
    if (!folder)
        return 0;
    int found = 0;
    for (struct dirent *entry; !found && (entry = readdir(folder)) != NULL;)
    {
        const size_t name_length = strlen(entry->d_name);
        if (name_length < 5 || strcmp(entry->d_name + name_length - 4, ".lpl") != 0)
            continue;
        char path[PS5_GAME_PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", playlists, entry->d_name);
        FILE *file = fopen(path, "rb");
        if (!file)
            continue;
        char *text = NULL;
        long length = 0;
        if (fseek(file, 0, SEEK_END) == 0 && (length = ftell(file)) > 0 && length < (32L << 20) &&
            fseek(file, 0, SEEK_SET) == 0 && (text = (char *)malloc((size_t)length)) != NULL &&
            fread(text, 1, (size_t)length, file) == (size_t)length)
            found = playlist_core(text, (size_t)length, content, core, size);
        free(text);
        fclose(file);
    }
    closedir(folder);
    return found;
}

/* --- the frontend's side -------------------------------------------------------- */

int ps5_game_launch(struct ps5_game *request)
{
    if (ps5_game_check(request) != 0)
        return -1;
    if (ps5_game_write(PS5_GAME_REQUEST_PATH, request, 0) != 0)
    {
        set_error(request, "the request could not be written");
        return -1;
    }
    const char *const arguments[] = {"--ps5-mode=game", NULL};
    fflush(NULL);
    const int result = sceSystemServiceLoadExec(PS5_GAME_EBOOT, arguments);
    /* Accepted, LoadExec returns and the shell replaces this process a moment later. */
    if (result >= 0)
    {
        const struct timespec tenth = {0, 100000000L};
        for (int waited = 0; waited < 600; waited++)
            nanosleep(&tenth, NULL);
    }
    remove(PS5_GAME_REQUEST_PATH);
    snprintf(request->error, sizeof(request->error),
             "LoadExec of " PS5_GAME_EBOOT " did not restart the title (%d)", result);
    return -1;
}

int ps5_game_take_result(const char *frontend, struct ps5_game *result)
{
    int kind = 0;
    if (ps5_game_read(PS5_GAME_RESULT_PATH, result, &kind) != 0 || kind != 1 ||
        strcmp(result->frontend, frontend) != 0)
        return 0;
    remove(PS5_GAME_RESULT_PATH);
    return 1;
}
