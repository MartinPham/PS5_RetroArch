/* PS5 RetroArch - EmulationStation's systems and game lists, from RetroArch's playlists.
 *
 * Copyright (C) 2026 Mihawk
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * ES-DE keeps no library of its own here: each time it starts (main_ps5.cpp, before
 * ES-DE reads anything), the game library every frontend shares (src/ps5_library.h)
 * is read from RetroArch's playlists and core info, and ES-DE's files are written
 * from it:
 *
 *   - ES-DE/custom_systems/es_systems.xml: a system for each of the library's, named
 *     as ES-DE names the platform (its full name, platform and theme taken from
 *     ES-DE's own systems file, resources/systems/unix/es_systems.xml), its path the
 *     folder of its games, its command game mode's: "/app0/eboot.bin -L <core> %ROM%";
 *   - ES-DE/gamelists/<system>/gamelist.xml: the system's games. A game ES-DE already
 *     lists keeps everything ES-DE knows of it (play count and time, favourite,
 *     scraped details); a new one gets its playlist label; one no playlist has any
 *     more is left out.
 *
 * ES-DE then runs with --gamelist-only, so its systems hold exactly those games, as
 * RetroArch's playlists do, and nothing is scanned for.
 */
#include "library_ps5.h"

#include "ps5_library.h"

#include <pugixml.hpp>

#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <sys/stat.h>

namespace
{
struct Reference
{
    std::string fullname, platform, theme;
};

/* ES-DE's own systems: name -> full name, platform, theme. */
std::map<std::string, Reference> read_reference(const std::string &path)
{
    std::map<std::string, Reference> systems;
    pugi::xml_document document;
    if (!document.load_file(path.c_str()))
        return systems;
    for (pugi::xml_node system : document.child("systemList").children("system"))
        systems[system.child_value("name")] = {system.child_value("fullname"), system.child_value("platform"),
                                               system.child_value("theme")};
    return systems;
}

std::string extensions(const char *list)
{
    std::string out;
    for (const char *at = list; at && *at;)
    {
        const char *bar = std::strchr(at, '|');
        const std::string extension(at, bar ? static_cast<size_t>(bar - at) : std::strlen(at));
        std::string upper = extension;
        for (char &c : upper)
            c = static_cast<char>(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c);
        out += (out.empty() ? "." : " .") + extension;
        if (upper != extension)
            out += " ." + upper;
        at = bar ? bar + 1 : nullptr;
    }
    return out;
}

/* A game list's path, absolute: "./x" is relative to the system's folder. */
std::string absolute(const std::string &folder, const std::string &path)
{
    if (path.rfind("./", 0) == 0)
        return (folder == "/" ? "" : folder) + "/" + path.substr(2);
    return path;
}

bool save(pugi::xml_document &document, const std::string &path)
{
    const std::string temporary = path + ".tmp";
    if (!document.save_file(temporary.c_str(), "\t", pugi::format_default, pugi::encoding_utf8))
        return false;
    chmod(temporary.c_str(), 0666);
    if (std::rename(temporary.c_str(), path.c_str()) != 0)
    {
        std::remove(temporary.c_str());
        return false;
    }
    return true;
}
} // namespace

extern "C" int ps5_esde_write_library_to(const struct ps5_esde_library_paths *paths, char *summary,
                                         size_t summary_size)
{
    struct ps5_library library;
    if (ps5_library_load(&library, paths->playlists, paths->info, paths->cores) != 0)
    {
        ps5_library_free(&library);
        std::snprintf(summary, summary_size, "the library could not be read (memory)");
        return -1;
    }
    const std::map<std::string, Reference> reference = read_reference(paths->reference);
    const std::string data = paths->data;
    mkdir((data + "/custom_systems").c_str(), 0777);
    mkdir((data + "/gamelists").c_str(), 0777);

    pugi::xml_document systems;
    systems.append_child(pugi::node_comment)
        .set_value(" Written by es-de.bin each time it starts, from RetroArch's playlists (src/ps5_library.h). "
                   "Changes here are lost: change the playlists in RetroArch. ");
    pugi::xml_node list = systems.append_child("systemList");
    std::set<std::string> names;
    size_t games_written = 0, games_kept = 0, failures = 0;
    for (size_t s = 0; s < library.system_count; s++)
    {
        const struct ps5_library_system &system = library.systems[s];
        std::string name = system.id;
        for (int suffix = 2; names.count(name); suffix++)
            name = std::string(system.id) + "-" + std::to_string(suffix);
        names.insert(name);
        const auto known = reference.find(system.id);
        pugi::xml_node entry = list.append_child("system");
        entry.append_child("name").text().set(name.c_str());
        entry.append_child("fullname").text().set(known != reference.end() ? known->second.fullname.c_str()
                                                                            : system.name);
        entry.append_child("path").text().set(system.folder);
        entry.append_child("extension").text().set(extensions(system.extensions).c_str());
        const std::string command = system.core[0]
                                        ? std::string("/app0/eboot.bin -L ") + system.core + " %ROM%"
                                        : std::string("/app0/eboot.bin %ROM%");
        pugi::xml_node launch = entry.append_child("command");
        launch.append_attribute("label").set_value("RetroArch");
        launch.text().set(command.c_str());
        entry.append_child("platform").text().set(known != reference.end() ? known->second.platform.c_str()
                                                                            : system.id);
        entry.append_child("theme").text().set(known != reference.end() ? known->second.theme.c_str() : system.id);

        /* Its game list: what ES-DE knows of each game kept, the playlists' games only. */
        const std::string folder = data + "/gamelists/" + name;
        const std::string gamelist = folder + "/gamelist.xml";
        mkdir(folder.c_str(), 0777);
        pugi::xml_document previous, games;
        previous.load_file(gamelist.c_str());
        std::map<std::string, pugi::xml_node> known_games;
        for (pugi::xml_node game : previous.child("gameList").children("game"))
            known_games[absolute(system.folder, game.child_value("path"))] = game;
        pugi::xml_node root = games.append_child("gameList");
        for (pugi::xml_node node : previous.child("gameList").children("folder"))
            root.append_copy(node);
        const std::string prefix = std::string(system.folder) == "/" ? "/" : std::string(system.folder) + "/";
        for (size_t g = 0; g < system.game_count; g++)
        {
            const struct ps5_library_game &game = library.games[system.first_game + g];
            const std::string path = game.path;
            const std::string relative =
                "./" + (path.rfind(prefix, 0) == 0 ? path.substr(prefix.size()) : path.substr(path.rfind('/') + 1));
            const auto previous_game = known_games.find(path);
            if (previous_game != known_games.end())
            {
                pugi::xml_node copy = root.append_copy(previous_game->second);
                copy.child("path").text().set(relative.c_str());
                if (!copy.child("name"))
                    copy.append_child("name").text().set(game.label);
                games_kept++;
            }
            else
            {
                pugi::xml_node node = root.append_child("game");
                node.append_child("path").text().set(relative.c_str());
                node.append_child("name").text().set(game.label);
            }
            games_written++;
        }
        if (!save(games, gamelist))
            failures++;
    }
    if (!save(systems, data + "/custom_systems/es_systems.xml"))
        failures++;
    const size_t system_count = library.system_count;
    std::snprintf(summary, summary_size, "%zu systems, %zu games (%zu with ES-DE's details kept), %zu cores, %zu failures",
                  system_count, games_written, games_kept, library.core_count, failures);
    ps5_library_free(&library);
    return failures ? -1 : static_cast<int>(system_count);
}

extern "C" int ps5_esde_write_library(char *summary, size_t summary_size)
{
    const struct ps5_esde_library_paths paths = {PS5_LIBRARY_PLAYLISTS, PS5_LIBRARY_INFO, PS5_LIBRARY_CORES,
                                                 "/app0/es-de/resources/systems/unix/es_systems.xml",
                                                 "/app0/es-de/ES-DE"};
    return ps5_esde_write_library_to(&paths, summary, summary_size);
}
