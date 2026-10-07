/* EmulationStation's systems and game lists from RetroArch's playlists
 * (frontends/es-de/ps5/library_ps5.cpp), on the host: ES-DE's names, full names and
 * themes for the platforms it knows, a system of its own for one it does not, game
 * mode's commands, and game lists that keep what ES-DE knows of a game, add new games
 * and drop the ones no playlist has, and what RetroArch remembers of play merged in:
 * favourites from either frontend, last played times and counts, and the collections
 * enabled once. argv[1] is a scratch folder. */
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>

#include <pugixml.hpp>

#include "../frontends/es-de/ps5/library_ps5.h"

extern "C" int sceSystemServiceLoadExec(const char *, const char *const *)
{
    return -1;
}

static void write_text(const std::string &path, const std::string &text)
{
    std::FILE *file = std::fopen(path.c_str(), "wb");
    assert(file);
    std::fputs(text.c_str(), file);
    std::fclose(file);
}

static pugi::xml_node system_named(pugi::xml_document &document, const char *name)
{
    for (pugi::xml_node system : document.child("systemList").children("system"))
        if (std::strcmp(system.child_value("name"), name) == 0)
            return system;
    return pugi::xml_node();
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    const std::string dir = argv[1];
    for (const char *folder : {"/cores", "/info", "/playlists", "/playlists/builtin",
                               "/playlists/logs", "/playlists/logs/Snes9x", "/data",
                               "/data/gamelists", "/data/gamelists/snes", "/data/settings"})
        mkdir((dir + folder).c_str(), 0777);
    write_text(dir + "/cores/snes9x_libretro.so", "");
    write_text(dir + "/info/snes9x_libretro.info",
               "display_name = \"Snes9x\"\ncorename = \"Snes9x\"\n"
               "database = \"Nintendo - Super Nintendo Entertainment System\"\n"
               "supported_extensions = \"smc|sfc\"\n");
    write_text(dir + "/reference.xml",
               "<?xml version=\"1.0\"?>\n<systemList>\n"
               "<system><name>snes</name><fullname>Nintendo SNES (Super Nintendo)</fullname>"
               "<path>%ROMPATH%/snes</path><extension>.sfc .SFC</extension><command>x</command>"
               "<platform>snes</platform><theme>snes</theme></system>\n</systemList>\n");
    const std::string snes = "/app0/content/SNES";
    write_text(
        dir + "/playlists/content.lpl",
        "{\"items\": ["
        "{\"path\": \"" +
            snes +
            "/Donkey Kong 2 (USA).zip\", \"label\": \"Donkey Kong 2\", \"core_path\": "
            "\"DETECT\", \"db_name\": \"Nintendo - Super Nintendo Entertainment System.lpl\"},"
            "{\"path\": \"" +
            snes +
            "/Hacks/Mario & Luigi.sfc\", \"label\": \"Mario & Luigi\", \"core_path\": "
            "\"DETECT\", \"db_name\": \"Nintendo - Super Nintendo Entertainment System.lpl\"},"
            "{\"path\": \"/app0/content/Homebrew/demo.bin\", \"label\": \"Demo\", \"core_path\": "
            "\"DETECT\", "
            "\"db_name\": \"content.lpl\"}]}");
    /* What ES-DE knows already: a play counted, a favourite, and a game gone from the playlists. */
    write_text(
        dir + "/data/gamelists/snes/gamelist.xml",
        "<?xml version=\"1.0\"?>\n<gameList>\n"
        "<game><path>./Donkey Kong 2 (USA).zip</path><name>DKC 2 (renamed in ES-DE)</name>"
        "<playcount>3</playcount><favorite>true</favorite></game>\n"
        "<game><path>./Gone (USA).zip</path><name>Gone</name><playcount>9</playcount></game>\n"
        "</gameList>\n");

    /* What RetroArch remembers: a favourite, the history, a runtime log; and ES-DE's
     * settings from before the collections default. */
    const std::string luigi = snes + "/Hacks/Mario & Luigi.sfc";
    write_text(dir + "/playlists/builtin/content_favorites.lpl",
               "{\"items\": [{\"path\": \"" + luigi + "\"}]}");
    write_text(dir + "/playlists/builtin/content_history.lpl",
               "{\"items\": [{\"path\": \"/app0/content/Homebrew/demo.bin\"}]}");
    write_text(dir + "/playlists/logs/Snes9x/Donkey Kong 2 (USA).lrtl",
               "{\"version\": \"1.0\", \"runtime\": \"0:02:05\", \"last_played\": "
               "\"2026-10-02 08:30:00\", \"play_count\": \"5\"}");
    write_text(dir + "/data/settings/es_settings.xml",
               "<?xml version=\"1.0\"?>\n<bool name=\"Other\" value=\"true\" />\n"
               "<string name=\"CollectionSystemsAuto\" value=\"\" />\n");

    const std::string playlists = dir + "/playlists", info = dir + "/info", cores = dir + "/cores",
                      reference = dir + "/reference.xml", data = dir + "/data";
    const struct ps5_esde_library_paths paths = {playlists.c_str(), info.c_str(), cores.c_str(),
                                                 reference.c_str(), data.c_str(), nullptr};
    char summary[256];
    assert(ps5_esde_write_library_to(&paths, summary, sizeof(summary)) == 2);
    assert(std::string(summary).find("2 systems, 3 games (1 with ES-DE's details kept)") == 0);

    pugi::xml_document systems;
    assert(systems.load_file((data + "/custom_systems/es_systems.xml").c_str()));
    pugi::xml_node known = system_named(systems, "snes");
    assert(known && std::string(known.child_value("fullname")) == "Nintendo SNES (Super Nintendo)");
    assert(std::string(known.child_value("path")) == snes &&
           std::string(known.child_value("theme")) == "snes");
    assert(std::string(known.child_value("command")) ==
           "/app0/eboot.bin -L " + cores + "/snes9x_libretro.so %ROM%");
    assert(std::string(known.child("command").attribute("label").value()) == "RetroArch");
    const std::string extensions = known.child_value("extension");
    assert(extensions.find(".zip .ZIP") != std::string::npos &&
           extensions.find(".sfc .SFC") != std::string::npos &&
           extensions.find(".smc .SMC") != std::string::npos);
    pugi::xml_node own = system_named(systems, "homebrew");
    assert(own && std::string(own.child_value("fullname")) == "Homebrew" &&
           std::string(own.child_value("theme")) == "homebrew" &&
           std::string(own.child_value("command")) == "/app0/eboot.bin %ROM%");

    pugi::xml_document games;
    assert(games.load_file((data + "/gamelists/snes/gamelist.xml").c_str()));
    int count = 0;
    bool kept = false, added = false;
    for (pugi::xml_node game : games.child("gameList").children("game"))
    {
        count++;
        const std::string path = game.child_value("path");
        if (path == "./Donkey Kong 2 (USA).zip") /* ES-DE's favourite kept; RetroArch's play */
            kept = std::string(game.child_value("name")) == "DKC 2 (renamed in ES-DE)" &&
                   std::string(game.child_value("playcount")) == "5" &&
                   std::string(game.child_value("playtime")) == "125" &&
                   std::string(game.child_value("lastplayed")) == "20261002T083000" &&
                   std::string(game.child_value("favorite")) == "true";
        if (path == "./Hacks/Mario & Luigi.sfc") /* RetroArch's favourite */
            added = std::string(game.child_value("name")) == "Mario & Luigi" &&
                    std::string(game.child_value("favorite")) == "true" &&
                    !game.child("lastplayed");
        assert(path != "./Gone (USA).zip");
    }
    assert(count == 2 && kept && added);
    pugi::xml_document homebrew;
    assert(homebrew.load_file((data + "/gamelists/homebrew/gamelist.xml").c_str()));
    assert(std::string(homebrew.child("gameList").child("game").child_value("path")) ==
           "./demo.bin");
    /* In the history only: a time from its place. */
    assert(std::strlen(homebrew.child("gameList").child("game").child_value("lastplayed")) == 15);
    assert(std::string(summary).find("2 favourites, 2 played") != std::string::npos);
    /* The collections, enabled once, beside the other settings. */
    pugi::xml_document settings;
    assert(settings.load_file((data + "/settings/es_settings.xml").c_str()));
    assert(std::string(settings.find_child_by_attribute("string", "name", "CollectionSystemsAuto")
                           .attribute("value")
                           .value()) == "favorites,recent");
    assert(settings.find_child_by_attribute("bool", "name", "Other"));

    /* Again: the same files. */
    assert(ps5_esde_write_library_to(&paths, summary, sizeof(summary)) == 2);
    assert(std::string(summary).find("2 systems, 3 games (3 with ES-DE's details kept)") == 0);

    /* Favourites in both frontends: in ES-DE the user takes DKC 2's away and Mario &
     * Luigi's too; RetroArch did not change either since, so ES-DE's choice stands. */
    const auto favourite = [&](const char *path)
    {
        pugi::xml_document list;
        assert(list.load_file((data + "/gamelists/snes/gamelist.xml").c_str()));
        for (pugi::xml_node game : list.child("gameList").children("game"))
            if (std::strcmp(game.child_value("path"), path) == 0)
                return std::strcmp(game.child_value("favorite"), "true") == 0;
        assert(!"a game the list should have");
        return false;
    };
    assert(favourite("./Donkey Kong 2 (USA).zip") && favourite("./Hacks/Mario & Luigi.sfc"));
    const auto unfavourite_in_esde = [&]()
    {
        pugi::xml_document list;
        assert(list.load_file((data + "/gamelists/snes/gamelist.xml").c_str()));
        for (pugi::xml_node game : list.child("gameList").children("game"))
            game.remove_child("favorite");
        list.save_file((data + "/gamelists/snes/gamelist.xml").c_str());
    };
    unfavourite_in_esde();
    assert(ps5_esde_write_library_to(&paths, summary, sizeof(summary)) == 2);
    assert(!favourite("./Donkey Kong 2 (USA).zip") && !favourite("./Hacks/Mario & Luigi.sfc"));
    /* RetroArch takes Mario & Luigi out and puts DKC 2 in: ES-DE follows. */
    write_text(dir + "/playlists/builtin/content_favorites.lpl",
               "{\"items\": [{\"path\": \"" + snes + "/Donkey Kong 2 (USA).zip\"}]}");
    assert(ps5_esde_write_library_to(&paths, summary, sizeof(summary)) == 2);
    assert(favourite("./Donkey Kong 2 (USA).zip") && !favourite("./Hacks/Mario & Luigi.sfc"));
    /* A later choice of the user's about the collections is kept. */
    write_text(dir + "/data/settings/es_settings.xml",
               "<?xml version=\"1.0\"?>\n<string name=\"CollectionSystemsAuto\" value=\"\" />\n");
    assert(ps5_esde_write_library_to(&paths, summary, sizeof(summary)) == 2);
    assert(settings.load_file((data + "/settings/es_settings.xml").c_str()));
    assert(std::string(settings.find_child_by_attribute("string", "name", "CollectionSystemsAuto")
                           .attribute("value")
                           .value())
               .empty());
    std::puts("esde_library: systems, full names, themes, commands, merged game lists, favourites "
              "from both frontends, RetroArch's play records and the collections PASS");
    return 0;
}
