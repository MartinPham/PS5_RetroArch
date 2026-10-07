/* Copyright (C) 2026 Mihawk; SPDX-License-Identifier: GPL-3.0-or-later
 * HTTP transport follows ps5-payload-dev/websrv: libmicrohttpd manages framing,
 * partial bodies, timeouts and connections. Routes are restricted to RetroArch.
 */
#include "webui_ps5.h"
#include "webui_update.h"
#include "webui_transfer.h"
#include "scraper.h"
#include "ps5_frontend_choice.h"
#include "../vendor/retroarch/libretro-common/include/libretro.h"
#include <microhttpd.h>
#include <algorithm>
#include <atomic>
#include <arpa/inet.h>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <map>
#include <memory>
#include <mutex>
#include <netinet/tcp.h>
#include <new>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <vector>

namespace
{
MHD_Daemon *web_daemon = nullptr;
std::string root_path, token;
std::mutex frontend_lock;
std::string frontend_name = "retroarch";
unsigned short listen_port;
constexpr uint64_t upload_limit = UINT64_C(64) * 1024 * 1024 * 1024;
struct Setting
{
    const char *key;
    const char *label;
    const char *kind;
    const char *initial;
    int min, max;
};
const Setting settings[] = {
    {"audio_volume", "Audio volume", "number", "0", -80, 12},
    {"input_rumble_gain", "Rumble strength", "number", "100", 0, 100},
    {"video_smooth", "Smooth image scaling", "bool", "false", 0, 0},
    {"video_vsync", "Vertical sync", "bool", "true", 0, 0},
    {"savestate_auto_save", "Save state when closing content", "bool", "false", 0, 0},
    {"savestate_auto_load", "Load state when opening content", "bool", "false", 0, 0},
    {"menu_show_advanced_settings", "Show advanced settings", "bool", "false", 0, 0},
    {"menu_driver", "Console menu", "menu", "xmb", 0, 0},
};
// The server answers on a pool of threads (ps5_webui_start). Transfers write on
// their own; everything else (settings, folders, the updater) runs under this lock,
// as it did on the one thread before.
std::mutex state_lock;
ps5_transfer::Sessions sessions;
std::atomic<unsigned> transfers_in_flight{0};
constexpr std::time_t session_idle_seconds = 15 * 60;
struct BatchState;
enum class Transfer
{
    none,
    single, // PUT /api/upload
    batch,  // PUT /api/upload/batch
    part,   // PUT /api/upload/part
    scraped // PUT /api/scraper/pc/media: a file the PC helper downloaded
};
struct Request
{
    Transfer transfer = Transfer::none;
    ps5_transfer::Writer writer;
    std::unique_ptr<BatchState> batch;
    std::shared_ptr<ps5_transfer::Session> session;
    bool replace = false;
    std::string temporary, destination, body;
    uint64_t received = 0, expected = 0, offset = 0;
    size_t body_limit = 16384;
    std::string job;
    unsigned task = 0;
    unsigned error = 0;
    const char *message = "";
    bool counted = false; // in transfers_in_flight
    ~Request();
};
std::string quote(const std::string &text)
{
    std::string out = "\"";
    for (unsigned char c : text)
    {
        if (c == '"' || c == '\\')
        {
            out += '\\';
            out += c;
        }
        else if (c < 32)
        {
            char escape[7];
            std::snprintf(escape, sizeof escape, "\\u%04x", c);
            out += escape;
        }
        else
            out += c;
    }
    return out + '"';
}
std::string read_file(const std::string &name, size_t limit)
{
    std::string out;
    int fd = open(name.c_str(), O_RDONLY | O_NOFOLLOW);
    if (fd < 0)
        return out;
    char buffer[4096];
    while (out.size() < limit)
    {
        ssize_t n = read(fd, buffer, std::min(sizeof buffer, limit - out.size()));
        if (n <= 0)
            break;
        out.append(buffer, size_t(n));
    }
    close(fd);
    return out;
}
std::string nonce()
{
    unsigned char bytes[16];
    arc4random_buf(bytes, sizeof bytes);
    std::string out;
    for (auto b : bytes)
    {
        char hex[3];
        std::snprintf(hex, sizeof hex, "%02x", b);
        out += hex;
    }
    return out;
}
MHD_Result respond(MHD_Connection *c, unsigned status, const std::string &body,
                   const char *type = "application/json")
{
    auto *response = MHD_create_response_from_buffer(body.size(), const_cast<char *>(body.data()),
                                                     MHD_RESPMEM_MUST_COPY);
    if (!response)
        return MHD_NO;
    MHD_add_response_header(response, "Content-Type", type);
    MHD_add_response_header(response, "Cache-Control", "no-store");
    MHD_add_response_header(response, "X-Content-Type-Options", "nosniff");
    MHD_add_response_header(response, "Referrer-Policy", "no-referrer");
    MHD_add_response_header(response, "Content-Security-Policy",
                            "default-src 'self'; connect-src 'self' https://api.github.com; "
                            "img-src 'self'; style-src 'self'; script-src 'self'; font-src 'self'; "
                            "frame-ancestors 'none'; base-uri 'none'; form-action 'self'");
    auto result = MHD_queue_response(c, status, response);
    MHD_destroy_response(response);
    return result;
}
MHD_Result error(MHD_Connection *c, unsigned status, const char *message)
{
    return respond(c, status, "{\"error\":" + quote(message) + "}");
}
const char *arg(MHD_Connection *c, const char *key)
{
    const char *v = MHD_lookup_connection_value(c, MHD_GET_ARGUMENT_KIND, key);
    return v ? v : "";
}
bool local_origin(MHD_Connection *c)
{
    const char *host = MHD_lookup_connection_value(c, MHD_HEADER_KIND, "Host");
    if (!host)
        return false;
    const auto *info = MHD_get_connection_info(c, MHD_CONNECTION_INFO_CONNECTION_FD);
    if (!info)
        return false;
    sockaddr_in addr{};
    socklen_t size = sizeof addr;
    if (getsockname(info->connect_fd, reinterpret_cast<sockaddr *>(&addr), &size) != 0 ||
        addr.sin_family != AF_INET)
        return false;
    char ip[INET_ADDRSTRLEN];
    if (!inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof ip))
        return false;
    const std::string expected = std::string(ip) + ":" + std::to_string(listen_port);
    if (host != expected)
        return false; // Literal console address also prevents DNS rebinding.
    const char *origin = MHD_lookup_connection_value(c, MHD_HEADER_KIND, "Origin");
    return !origin || std::string(origin) == "http://" + expected;
}
bool valid_path(const std::string &relative)
{
    if (relative.size() > 1024 || (!relative.empty() && relative.front() == '/'))
        return false;
    size_t start = 0;
    while (start < relative.size())
    {
        const auto end = relative.find('/', start);
        const auto part = relative.substr(start, end - start);
        if (part.empty() || part.front() == '.' || part.size() > 255)
            return false;
        for (unsigned char c : part)
            if (c < 32 || c == 127 || c == '\\' || c == ':')
                return false;
        if (end == std::string::npos)
            return true;
        start = end + 1;
        if (start == relative.size())
            return false;
    }
    return true;
}
// A title cannot use lstat reliably. Opening without following links preserves
// the path boundary and lets fstat obtain the native file type and size.
int content_stat(const char *path, struct stat *st)
{
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0)
        return -1;
    int result = fstat(fd, st);
    int saved_errno = errno;
    close(fd);
    errno = saved_errno;
    return result;
}
bool storage_space(struct statvfs &storage)
{
#ifdef __PROSPERO__
    // The SDK's statvfs compatibility answer is synthetic, not free disk space.
    (void)storage;
    return false;
#else
    return statvfs((root_path + "/content").c_str(), &storage) == 0;
#endif
}
// Refuse symlinks at every component. Only files below content/ are exposed.
bool content_path(const std::string &relative, std::string &absolute, bool new_leaf = false)
{
    if (!valid_path(relative))
        return false;
    absolute = root_path + "/content";
    struct stat st{};
    if (content_stat(absolute.c_str(), &st) || !S_ISDIR(st.st_mode))
        return false;
    size_t start = 0;
    while (start < relative.size())
    {
        auto end = relative.find('/', start);
        absolute += '/' + relative.substr(start, end - start);
        if (end == std::string::npos && new_leaf)
            return true;
        if (content_stat(absolute.c_str(), &st) || S_ISLNK(st.st_mode))
            return false;
        if (end != std::string::npos && !S_ISDIR(st.st_mode))
            return false;
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return true;
}
// A path below content/ for a new file, its missing folders created on the way
// (an upload of a folder's files, a batch's subfolders). The leaf is not checked.
bool content_parents(const std::string &relative, std::string &absolute)
{
    if (relative.empty() || !valid_path(relative))
        return false;
    absolute = root_path + "/content";
    struct stat st{};
    if (content_stat(absolute.c_str(), &st) || !S_ISDIR(st.st_mode))
        return false;
    size_t start = 0;
    for (;;)
    {
        const auto end = relative.find('/', start);
        absolute += '/' + relative.substr(start, end - start);
        if (end == std::string::npos)
            return true;
        if (content_stat(absolute.c_str(), &st))
        {
            if (errno != ENOENT || (mkdir(absolute.c_str(), 0755) && errno != EEXIST) ||
                content_stat(absolute.c_str(), &st))
                return false;
        }
        if (S_ISLNK(st.st_mode) || !S_ISDIR(st.st_mode))
            return false;
        start = end + 1;
    }
}
bool exists(const std::string &absolute)
{
    struct stat st{};
    return content_stat(absolute.c_str(), &st) == 0 || errno != ENOENT;
}
std::string trim(std::string s)
{
    auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return "";
    s = s.substr(first, s.find_last_not_of(" \t\r\n") - first + 1);
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
        s = s.substr(1, s.size() - 2);
    return s;
}
using Config = std::map<std::string, std::string>;
bool setting_key(const std::string &key)
{
    return !key.empty() && key.size() <= 160 &&
           key.find_first_not_of(
               "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-.") ==
               std::string::npos;
}
Config parse_config(const std::string &text)
{
    Config values;
    size_t start = 0;
    while (start < text.size())
    {
        auto end = text.find('\n', start);
        auto line = text.substr(start, end - start);
        auto eq = line.find('=');
        if (eq != std::string::npos)
        {
            auto key = trim(line.substr(0, eq));
            if (setting_key(key))
                values[key] = trim(line.substr(eq + 1));
        }
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return values;
}
Config read_config(const std::string &path)
{
    return parse_config(read_file(path, 2 * 1024 * 1024));
}
void overlay(Config &to, const Config &from)
{
    for (const auto &entry : from)
        to[entry.first] = entry.second;
}
bool write_config(const std::string &path, const Config &values, std::string text = {})
{
    for (const auto &entry : values)
        text += entry.first + " = \"" + entry.second + "\"\n";
    const std::string temporary = path + ".webui-" + nonce();
    int fd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (fd < 0)
        return false;
    size_t offset = 0;
    while (offset < text.size())
    {
        ssize_t n = write(fd, text.data() + offset, text.size() - offset);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        offset += size_t(n);
    }
    bool ok = offset == text.size() && fsync(fd) == 0;
    close(fd);
    if (ok)
        ok = rename(temporary.c_str(), path.c_str()) == 0;
    unlink(temporary.c_str());
    return ok;
}
// Only catalogs whose core binary is installed participate in the profile list.
std::vector<std::string> installed_cores()
{
    std::vector<std::string> cores;
    for (const auto &entry : read_config(root_path + "/webui/core-metadata/index.cfg"))
    {
        struct stat st{};
        if (!valid_path(entry.first) || entry.first.find('/') != std::string::npos ||
            !valid_path(entry.second) || entry.second.find('/') != std::string::npos)
            continue;
        if (!content_stat((root_path + "/cores/" + entry.first).c_str(), &st) &&
            S_ISREG(st.st_mode))
            cores.push_back(entry.second);
    }
    return cores;
}
std::vector<std::string> core_profiles()
{
    auto cores = installed_cores();
    DIR *dir = opendir((root_path + "/config").c_str());
    if (!dir)
        return cores;
    while (auto *entry = readdir(dir))
    {
        std::string name = entry->d_name;
        if (!valid_path(name) || name.find('/') != std::string::npos || name == "webui-cores")
            continue;
        struct stat st{};
        std::string folder = root_path + "/config/" + name;
        if (content_stat(folder.c_str(), &st) || !S_ISDIR(st.st_mode))
            continue;
        for (const char *ext : {".opt", ".cfg"})
            if (!content_stat((folder + '/' + name + ext).c_str(), &st) && S_ISREG(st.st_mode))
            {
                cores.push_back(name);
                break;
            }
    }
    closedir(dir);
    std::sort(cores.begin(), cores.end());
    cores.erase(std::unique(cores.begin(), cores.end()), cores.end());
    return cores;
}
Config global_values()
{
    Config values;
    for (const auto &s : settings)
        values[s.key] = s.initial;
    overlay(values, read_config(root_path + "/retroarch.cfg"));
    overlay(values, read_config(root_path + "/config/retroarch.cfg"));
    overlay(values, read_config(root_path + "/config/webui.cfg"));
    return values;
}
std::string update_status()
{
    const auto u = ps5_update::status();
    return "{\"state\":" + quote(u.state) + ",\"message\":" + quote(u.message) +
           ",\"tag\":" + quote(u.tag) + ",\"received\":" + std::to_string(u.received) + '}';
}
std::string bios_alerts()
{
    std::string out = "{\"alerts\":[";
    auto add = [&](const std::string &core, const std::string &description, const std::string &path,
                   const std::string &message)
    {
        if (out.back() != '[')
            out += ',';
        out += "{\"core\":" + quote(core) + ",\"title\":" + quote(description) +
               ",\"path\":" + quote(path) + ",\"message\":" + quote(message) + '}';
    };
    const auto global = global_values();
    for (const auto &entry : read_config(root_path + "/webui/core-metadata/index.cfg"))
    {
        if (!valid_path(entry.first) || entry.first.find('/') != std::string::npos ||
            entry.first == "rpcs3_libretro.so")
            continue;
        struct stat st{};
        if (content_stat((root_path + "/cores/" + entry.first).c_str(), &st))
            continue;
        auto values = global;
        overlay(values,
                read_config(root_path + "/config/" + entry.second + '/' + entry.second + ".cfg"));
        overlay(values, read_config(root_path + "/config/webui-cores/" + entry.second + ".cfg"));
        std::string directory = values["system_directory"];
        if (directory.empty() || directory == "default")
            directory = root_path + "/system";
        else if (directory.rfind(":/", 0) == 0)
            directory = root_path + directory.substr(1);
        else if (directory.rfind("/app0/", 0) == 0)
            directory = root_path + directory.substr(5);
        if (entry.second == "Beetle Saturn" && directory == root_path + "/system")
            directory += "/Saturn";
        if (directory.front() != '/')
        {
            add(entry.second, "System folder needs attention", directory,
                "Choose an absolute System/BIOS folder in this core's settings.");
            continue;
        }
        const auto info = read_config(root_path + "/info/" +
                                      entry.first.substr(0, entry.first.size() - 3) + ".info");
        for (const auto &firmware : info)
        {
            if (firmware.first.rfind("firmware", 0) || firmware.first.size() < 6 ||
                firmware.first.substr(firmware.first.size() - 5) != "_path")
                continue;
            const auto prefix = firmware.first.substr(0, firmware.first.size() - 5);
            auto optional = info.find(prefix + "_opt");
            if (optional == info.end() || optional->second != "false" ||
                !valid_path(firmware.second))
                continue;
            const std::string path = directory + '/' + firmware.second;
            bool present = !content_stat(path.c_str(), &st) &&
                           ((S_ISREG(st.st_mode) && st.st_size > 0) || S_ISDIR(st.st_mode));
            // A required BIOS directory exists after installation even when it is empty.
            if (present && S_ISDIR(st.st_mode))
            {
                present = false;
                DIR *folder = opendir(path.c_str());
                if (folder)
                {
                    while (auto *e = readdir(folder))
                    {
                        std::string name = e->d_name;
                        if (name.size() > 4 &&
                            (name.substr(name.size() - 4) == ".bin" ||
                             name.substr(name.size() - 4) == ".BIN") &&
                            !content_stat((path + '/' + name).c_str(), &st) &&
                            S_ISREG(st.st_mode) && st.st_size > 0)
                        {
                            present = true;
                            break;
                        }
                    }
                    closedir(folder);
                }
            }
            if (!present)
            {
                auto desc = info.find(prefix + "_desc");
                add(entry.second, desc == info.end() ? firmware.second : desc->second, path,
                    "Required firmware was not found here. Add your own BIOS/system files for the "
                    "regions and content you use. Presence only is checked; file authenticity is "
                    "not verified.");
            }
        }
    }
    auto update = ps5_update::status();
    if (update.state == "error")
        add("RetroArch update", "Update needs attention", "", update.message);
    return out + "],\"scope\":\"Installed cores and configured system folders\"}";
}

std::string revision(const Config &values)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    for (const auto &entry : values)
        for (unsigned char c : entry.first + '=' + entry.second + '\n')
            hash = (hash ^ c) * UINT64_C(1099511628211);
    return std::to_string(hash);
}
std::string catalog_revision(const std::string &core)
{
    // Catalogs include the staged core's binary hash, covering dynamic options
    // even when its static option table has not changed in a new release.
    return revision({{"catalog", read_file(root_path + "/webui/core-metadata/" + core + ".json",
                                           2 * 1024 * 1024)}});
}
std::string runtime_metadata(const std::string &core, const char *extension)
{
    const std::string folder = root_path + "/config/webui-metadata";
    struct stat st{};
    if (content_stat(folder.c_str(), &st) || !S_ISDIR(st.st_mode))
        return {};
    auto text = read_file(folder + '/' + core + extension, 2 * 1024 * 1024);
    const auto tag = catalog_revision(core);
    const auto prefix = std::strcmp(extension, ".json") == 0
                            ? "{\"catalogRevision\":" + quote(tag) + ','
                            : "# catalog-revision: " + tag + '\n';
    return text.compare(0, prefix.size(), prefix) == 0 ? text : std::string();
}
const char *value_kind(const std::string &value)
{
    if (value == "true" || value == "false")
        return "bool";
    char *end = nullptr;
    errno = 0;
    double n = std::strtod(value.c_str(), &end);
    if (!value.empty() && end && !*end && !errno && std::isfinite(n))
        return "number";
    return "text";
}
const char *setting_kind(const std::string &key, bool core_option)
{
    if (!core_option)
        for (const auto &s : settings)
            if (key == s.key)
                return std::strcmp(s.kind, "menu") == 0 ? "text" : s.kind;
    // Config files do not carry type metadata. Numeric-looking bindings and
    // enum choices remain strings, so changing their value cannot change type.
    return "text";
}
bool valid_setting_value(const std::string &value, const std::string &kind)
{
    if (value.size() > 4096)
        return false;
    // Config values are quoted. Reject syntax that could escape that value.
    for (unsigned char c : value)
        if (c < 32 || c == 127 || c == '"' || c == '\\')
            return false;
    return kind == "text" || kind == value_kind(value);
}
MHD_Result config_editor(MHD_Connection *c, const std::string &method, const std::string &body)
{
    const std::string scope = arg(c, "scope"), core = arg(c, "core");
    const bool global = scope == "global", options = scope == "core-options";
    if (!global && !options && scope != "core-settings")
        return error(c, 400, "Choose global settings or a core profile.");
    std::string path = root_path + "/config/webui.cfg";
    Config baseline = global_values(), saved;
    if (!global)
    {
        auto cores = core_profiles();
        if (std::find(cores.begin(), cores.end(), core) == cores.end())
            return error(c, 404, "This core is not installed and has no saved profile.");
        std::string ext = options ? ".opt" : ".cfg";
        if (options)
        {
            baseline = parse_config(runtime_metadata(core, ".opt"));
            overlay(baseline, read_config(root_path + "/webui/core-metadata/" + core + ".opt"));
        }
        overlay(baseline, read_config(root_path + "/config/" + core + '/' + core + ext));
        path = root_path + "/config/webui-cores/" + core + ext;
    }
    saved = read_config(path);
    overlay(baseline, saved);
    const std::string tag = revision(baseline);
    if (method == "GET")
    {
        std::string out = "{\"revision\":" + quote(tag) + ",\"settings\":[";
        for (const auto &entry : baseline)
        {
            if (out.back() != '[')
                out += ',';
            out += "{\"key\":" + quote(entry.first) + ",\"value\":" + quote(entry.second) +
                   ",\"kind\":" + quote(setting_kind(entry.first, options)) + '}';
        }
        return respond(c, 200, out + "],\"apply\":\"next_launch\"}");
    }
    if (method != "POST")
        return error(c, 405, "This action is not supported.");
    const char *expected = MHD_lookup_connection_value(c, MHD_HEADER_KIND, "X-RetroArch-Revision");
    if (!expected || tag != expected)
        return error(c, 409,
                     "Settings changed since this page loaded. Refresh before saving again.");
    size_t start = 0;
    unsigned changed = 0;
    while (start < body.size())
    {
        auto end = body.find('\n', start), eq = body.find('=', start);
        if (eq == std::string::npos || (end != std::string::npos && eq > end))
            return error(c, 400, "Invalid settings. Reload the page and try again.");
        auto key = body.substr(start, eq - start);
        auto value = body.substr(eq + 1, end == std::string::npos ? end : end - eq - 1);
        auto original = baseline.find(key);
        if (original == baseline.end() || !valid_setting_value(value, setting_kind(key, options)))
            return error(
                c, 400, "Use an existing setting and a valid value without quotes or line breaks.");
        if (!options)
            for (const auto &s : settings)
                if (key == s.key)
                {
                    if (std::strcmp(s.kind, "menu") == 0 && value != "xmb" && value != "rgui")
                        return error(c, 400, "Choose XMB or RGUI for the console menu.");
                    if (std::strcmp(s.kind, "number") == 0 &&
                        (std::strtod(value.c_str(), nullptr) < s.min ||
                         std::strtod(value.c_str(), nullptr) > s.max))
                        return error(c, 400, "A setting is outside its supported range.");
                }
        saved[key] = value;
        ++changed;
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    if (!changed)
        return error(c, 400, "No settings were provided.");
    if (!global)
    {
        std::string folder = root_path + "/config/webui-cores";
        mkdir(folder.c_str(), 0755);
        struct stat st{};
        if (content_stat(folder.c_str(), &st) || !S_ISDIR(st.st_mode))
            return error(c, 500, "The settings folder is not available.");
    }
    return write_config(path, saved)
               ? respond(c, 200, "{\"saved\":true,\"apply\":\"next_launch\"}")
               : error(c, 500, "Settings could not be saved. Check console storage.");
}
void apply_core_settings()
{
    for (const auto &core : core_profiles())
        for (const char *ext : {".opt", ".cfg"})
        {
            auto saved = read_config(root_path + "/config/webui-cores/" + core + ext);
            if (saved.empty())
                continue;
            const std::string folder = root_path + "/config/" + core;
            mkdir(folder.c_str(), 0755);
            struct stat st{};
            if (content_stat(folder.c_str(), &st) || !S_ISDIR(st.st_mode))
                continue;
            std::string destination = folder + '/' + core + ext;
            auto values = read_config(destination);
            overlay(values, saved);
            if (!write_config(destination, values))
                std::fprintf(stderr, "webui: could not apply saved core settings\n");
            else
            {
                // These two ports retire pre-profile option files on first use.
                // A deliberate WebUI edit is already a user profile: do not let
                // that one-time migration rename it away when the core starts.
                if (std::strcmp(ext, ".opt") == 0 && (core == "PPSSPP" || core == "dolphin-emu"))
                {
                    const std::string marker = folder + "/ps5-default-profile-v1";
                    int fd = open(marker.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
                    if (fd >= 0)
                        close(fd);
                    else if (errno != EEXIST)
                    {
                        std::fprintf(stderr, "webui: could not preserve first-use core profile\n");
                        continue; // Keep the pending edit for a later retry.
                    }
                }
                if (unlink((root_path + "/config/webui-cores/" + core + ext).c_str()))
                    std::fprintf(stderr, "webui: could not clear applied core settings\n");
            }
        }
}
Config config_values(bool overrides_only = false)
{
    return overrides_only ? read_config(root_path + "/config/webui.cfg") : global_values();
}
MHD_Result get_settings(MHD_Connection *c)
{
    auto values = config_values();
    std::string out = "{\"settings\":[";
    for (const auto &s : settings)
    {
        if (out.back() != '[')
            out += ',';
        out += "{\"key\":" + quote(s.key) + ",\"label\":" + quote(s.label) +
               ",\"kind\":" + quote(s.kind) + ",\"value\":" + quote(values[s.key]) +
               ",\"min\":" + std::to_string(s.min) + ",\"max\":" + std::to_string(s.max) + '}';
    }
    // The frontend the title starts with is the title's, not RetroArch's: it lives in
    // config/frontend.cfg (src/ps5_frontend_choice.h), never in webui.cfg.
    out +=
        ",{\"key\":\"frontend_start\",\"label\":\"Start with\",\"kind\":\"frontend\",\"value\":" +
        quote(ps5_frontend_choice_read((root_path + "/config/frontend.cfg").c_str())) +
        ",\"min\":0,\"max\":0}";
    return respond(c, 200, out + "],\"apply\":\"next_launch\"}");
}
MHD_Result save_settings(MHD_Connection *c, const std::string &body)
{
    // A bounded, plain key=value body: no arbitrary config paths or keys.
    auto values = config_values(true);
    size_t start = 0;
    unsigned changed = 0;
    const char *frontend = nullptr;
    while (start < body.size())
    {
        auto end = body.find('\n', start);
        auto line = body.substr(start, end - start);
        auto eq = line.find('=');
        if (eq == std::string::npos)
            return error(c, 400, "Invalid settings. Reload the page and try again.");
        auto key = line.substr(0, eq), value = line.substr(eq + 1);
        if (key == "frontend_start")
        {
            frontend = ps5_frontend_choice_name(value.c_str());
            if (!frontend)
                return error(c, 400, "A setting is outside its supported range.");
            ++changed;
            if (end == std::string::npos)
                break;
            start = end + 1;
            continue;
        }
        const Setting *setting = nullptr;
        for (auto &s : settings)
            if (key == s.key)
                setting = &s;
        if (!setting)
            return error(c, 400, "This setting cannot be changed through the WebUI.");
        bool valid = false;
        if (std::strcmp(setting->kind, "bool") == 0)
            valid = value == "true" || value == "false";
        else if (std::strcmp(setting->kind, "menu") == 0)
            valid = value == "xmb" || value == "rgui";
        else
        {
            char *tail = nullptr;
            errno = 0;
            long n = std::strtol(value.c_str(), &tail, 10);
            valid = !value.empty() && !errno && !*tail && n >= setting->min && n <= setting->max;
        }
        if (!valid)
            return error(c, 400, "A setting is outside its supported range.");
        values[key] = value;
        ++changed;
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    if (!changed)
        return error(c, 400, "No settings were provided.");
    if (frontend &&
        ps5_frontend_choice_write((root_path + "/config/frontend.cfg").c_str(), frontend) != 0)
        return error(c, 500, "Settings could not be saved. Check console storage.");
    if (frontend && changed == 1)
        return respond(c, 200, "{\"saved\":true,\"apply\":\"next_launch\"}");
    // Keep advanced global overrides when updating a quick setting.
    return write_config(root_path + "/config/webui.cfg", values)
               ? respond(c, 200, "{\"saved\":true,\"apply\":\"next_launch\"}")
               : error(c, 500, "Settings could not be saved. Check console storage.");
}

MHD_Result list_content(MHD_Connection *c)
{
    const std::string relative = arg(c, "path");
    std::string path;
    if (!content_path(relative, path))
        return error(c, 400, "Choose a folder inside RetroArch content.");
    DIR *dir = opendir(path.c_str());
    if (!dir)
        return error(c, 404, "This folder is not available.");
    struct Entry
    {
        std::string name;
        bool folder;
        uint64_t size;
    };
    std::vector<Entry> entries;
    bool truncated = false;
    while (auto *item = readdir(dir))
    {
        std::string name = item->d_name;
        if (!valid_path(name) || name.find('/') != std::string::npos)
            continue;
        struct stat st{};
        if (content_stat((path + '/' + name).c_str(), &st) ||
            (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode)))
            continue;
        if (entries.size() >= 10000)
        {
            truncated = true;
            break;
        }
        entries.push_back({name, S_ISDIR(st.st_mode), uint64_t(st.st_size)});
    }
    closedir(dir);
    std::sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b)
              { return a.folder != b.folder ? a.folder > b.folder : a.name < b.name; });
    std::string out = "{\"path\":" + quote(relative) + ",\"entries\":[";
    for (const auto &e : entries)
    {
        if (out.back() != '[')
            out += ',';
        out += "{\"name\":" + quote(e.name) + ",\"directory\":" + (e.folder ? "true" : "false") +
               ",\"size\":" + std::to_string(e.size) + '}';
    }
    return respond(c, 200, out + "],\"truncated\":" + (truncated ? "true" : "false") + '}');
}
// A download is read in blocks rather than handed to MHD as a descriptor: MHD sends a
// descriptor with sendfile, which on the console stopped after its first 33 KB
// (the daemon, 2026-10-07: curl got 33,300 of 1.9 GB and a broken transfer).
struct DownloadSource
{
    int fd;
    uint64_t start;
};
ssize_t read_download(void *cls, uint64_t position, char *buffer, size_t size)
{
    const auto *source = static_cast<DownloadSource *>(cls);
    const ssize_t got =
        pread(source->fd, buffer, size, static_cast<off_t>(source->start + position));
    if (got == 0)
        return MHD_CONTENT_READER_END_OF_STREAM;
    return got < 0 ? MHD_CONTENT_READER_END_WITH_ERROR : got;
}
void close_download(void *cls)
{
    auto *source = static_cast<DownloadSource *>(cls);
    close(source->fd);
    delete source;
}
MHD_Result serve_file(MHD_Connection *c, const std::string &path, const char *type,
                      bool attachment);
MHD_Result download(MHD_Connection *c)
{
    std::string path;
    if (!content_path(arg(c, "path"), path))
        return error(c, 400, "Invalid content path.");
    return serve_file(c, path, "application/octet-stream", true);
}
// A file streamed in 1 MiB blocks, one Range honoured: a content download (attachment)
// or a stored image or video the page shows.
MHD_Result serve_file(MHD_Connection *c, const std::string &path, const char *type, bool attachment)
{
    int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW);
    struct stat st{};
    if (fd < 0)
        return error(c, 404, "This file is not available.");
    if (fstat(fd, &st) || !S_ISREG(st.st_mode))
    {
        close(fd);
        return error(c, 400, "Choose a file to download.");
    }
    // One byte range (Range: bytes=a-b), so a client can fetch a large file on several
    // connections or resume one.
    const uint64_t size = static_cast<uint64_t>(st.st_size);
    uint64_t start = 0, end = size;
    const char *range = MHD_lookup_connection_value(c, MHD_HEADER_KIND, "Range");
    const bool partial = range && ps5_transfer::parse_range(range, size, start, end);
    if (range && !partial && size)
    {
        close(fd);
        return error(c, 416, "This part of the file does not exist.");
    }
    auto *source = new (std::nothrow) DownloadSource{fd, start};
    auto *response = source ? MHD_create_response_from_callback(
                                  end - start, 1024 * 1024, read_download, source, close_download)
                            : nullptr;
    if (!response)
    {
        delete source;
        close(fd);
        return MHD_NO;
    }
    MHD_add_response_header(response, "Accept-Ranges", "bytes");
    if (partial)
    {
        const std::string content_range = "bytes " + std::to_string(start) + '-' +
                                          std::to_string(end - 1) + '/' + std::to_string(size);
        MHD_add_response_header(response, "Content-Range", content_range.c_str());
    }
    if (attachment)
    {
        const std::string disposition =
            "attachment; filename=" + quote(path.substr(path.find_last_of('/') + 1));
        MHD_add_response_header(response, "Content-Disposition", disposition.c_str());
    }
    MHD_add_response_header(response, "Content-Type", type);
    MHD_add_response_header(response, "X-Content-Type-Options", "nosniff");
    MHD_add_response_header(response, "Cache-Control", attachment ? "no-store" : "no-cache");
    auto result = MHD_queue_response(c, partial ? 206 : 200, response);
    MHD_destroy_response(response);
    return result;
}
// Many small files in one request (src/webui_transfer.h, the batch stream): each is
// written under a temporary name and renamed when whole; one that exists is skipped
// unless replace. Results are counted, not answered one by one.
struct BatchUpload final : ps5_transfer::BatchSink
{
    std::string folder; // below content/, may be empty
    bool replace = false, sync = true;
    ps5_transfer::Writer writer;
    std::string name, temporary, destination;
    uint64_t size = 0;
    unsigned written = 0, skipped = 0;
    uint64_t bytes = 0;
    std::vector<std::pair<std::string, std::string>> failed;
    std::vector<std::string> skipped_names; // the first 1024
    void skip()
    {
        ++skipped;
        if (skipped_names.size() < 1024)
            skipped_names.push_back(name);
    }
    bool fail(const char *why)
    {
        if (failed.size() < 64)
            failed.emplace_back(name, why);
        else
            failed.back().second = "and more";
        const int fd = writer.release();
        if (fd >= 0)
            close(fd);
        if (!temporary.empty())
            unlink(temporary.c_str());
        temporary.clear();
        return false;
    }
    bool begin(const std::string &record, uint64_t record_size) override
    {
        name = record;
        size = record_size;
        temporary.clear();
        if (record_size > upload_limit ||
            !content_parents(folder.empty() ? record : folder + '/' + record, destination))
            return fail("invalid name");
        if (!replace && exists(destination))
        {
            skip();
            return false;
        }
        temporary = destination.substr(0, destination.find_last_of('/') + 1) + ".upload-" + nonce();
        return writer.create(temporary) || fail("cannot create");
    }
    bool data(const char *bytes_in, size_t count) override
    {
        return writer.write(bytes_in, count) || fail("cannot write (storage full?)");
    }
    void end() override
    {
        const bool flushed = writer.flush();
        const uint64_t got = writer.written();
        const int fd = writer.release();
        if (!flushed || fd < 0 || got != size)
        {
            if (fd >= 0)
                close(fd);
            fail("incomplete");
            return;
        }
        const auto result = ps5_transfer::commit(fd, temporary, destination, replace, sync);
        close(fd);
        if (result == ps5_transfer::Commit::done)
        {
            temporary.clear();
            ++written;
            bytes += size;
        }
        else if (result == ps5_transfer::Commit::exists)
        {
            unlink(temporary.c_str());
            temporary.clear();
            skip();
        }
        else
            fail("cannot finish");
    }
    ~BatchUpload() override
    {
        const int fd = writer.release();
        if (fd >= 0)
            close(fd);
        if (!temporary.empty())
            unlink(temporary.c_str());
    }
};
struct BatchState
{
    BatchUpload upload;
    ps5_transfer::BatchReader reader{upload};
};
Request::~Request()
{
    if (counted)
        --transfers_in_flight;
    if (transfer == Transfer::part && session)
    {
        std::lock_guard<std::mutex> guard(session->lock);
        if (--session->writers == 0 && session->closing && session->fd >= 0)
        {
            close(session->fd);
            session->fd = -1;
        }
    }
    if (!temporary.empty())
    {
        unlink(temporary.c_str());
        /* A helper's upload cut short: the file is handed out again at once. */
        if (transfer == Transfer::scraped)
            ps5_scraper::pc_delivered(job, task, false, 0);
    }
}
bool content_length(MHD_Connection *c, uint64_t &length)
{
    const char *value = MHD_lookup_connection_value(c, MHD_HEADER_KIND, "Content-Length");
    char *end = nullptr;
    errno = 0;
    if (!value || !*value || *value == '-')
        return false;
    length = std::strtoull(value, &end, 10);
    return !errno && !*end;
}
void prepare_upload(MHD_Connection *c, Request &r)
{
    auto fail = [&](unsigned code, const char *message)
    {
        r.error = code;
        r.message = message;
    };
    const std::string relative = arg(c, "path");
    if (relative.empty() || !content_path(relative, r.destination, true))
    {
        fail(400, "Choose a valid content folder and filename.");
        return;
    }
    r.transfer = Transfer::single;
    r.replace = std::strcmp(arg(c, "existing"), "replace") == 0;
    if (!r.replace && exists(r.destination))
    {
        fail(409, "A file with this name already exists. Rename your file first.");
        return;
    }
    const char *length = MHD_lookup_connection_value(c, MHD_HEADER_KIND, "Content-Length");
    char *end = nullptr;
    errno = 0;
    if (!length || !*length || *length == '-')
    {
        fail(411, "A file size is required.");
        return;
    }
    r.expected = std::strtoull(length, &end, 10);
    if (errno || *end || r.expected > upload_limit)
    {
        fail(413, "Files must be 64 GiB or smaller.");
        return;
    }
    struct statvfs storage{};
    if (storage_space(storage) && r.expected > uint64_t(storage.f_bavail) * storage.f_frsize)
    {
        fail(507, "There is not enough free space on the console.");
        return;
    }
    r.temporary =
        r.destination.substr(0, r.destination.find_last_of('/') + 1) + ".upload-" + nonce();
    if (!r.writer.create(r.temporary))
    {
        r.temporary.clear();
        fail(507, "Could not create the upload. Check console storage.");
    }
}
void prepare_batch(MHD_Connection *c, Request &r)
{
    r.transfer = Transfer::batch;
    if (!content_length(c, r.expected) || r.expected > upload_limit)
    {
        r.error = 411;
        r.message = "A batch needs its size, at most 64 GiB.";
        return;
    }
    auto state = std::make_unique<BatchState>();
    state->upload.folder = arg(c, "folder");
    state->upload.replace = std::strcmp(arg(c, "existing"), "replace") == 0;
    state->upload.sync = std::strcmp(arg(c, "sync"), "0") != 0;
    if (!state->upload.folder.empty() && !valid_path(state->upload.folder))
    {
        r.error = 400;
        r.message = "Choose a valid content folder.";
        return;
    }
    r.batch = std::move(state);
}
void prepare_part(MHD_Connection *c, Request &r)
{
    r.transfer = Transfer::part;
    r.session = sessions.find(arg(c, "id"));
    char *end = nullptr;
    const char *offset = arg(c, "offset");
    errno = 0;
    r.offset = std::strtoull(offset, &end, 10);
    if (!r.session)
    {
        r.error = 404;
        r.message = "This upload is no longer open. Start it again.";
        return;
    }
    if (!*offset || errno || *end || !content_length(c, r.expected) || r.offset > r.session->size ||
        r.expected > r.session->size - r.offset)
    {
        r.session.reset();
        r.error = 416;
        r.message = "This part does not fit the file.";
        return;
    }
    std::lock_guard<std::mutex> guard(r.session->lock);
    if (r.session->closing || r.session->fd < 0)
    {
        r.session.reset();
        r.error = 409;
        r.message = "This upload was closed.";
        return;
    }
    ++r.session->writers;
    r.session->touched = std::time(nullptr);
    r.writer.attach(r.session->fd, r.offset);
}
std::string names(const std::vector<std::string> &list)
{
    std::string out = "[";
    for (const auto &name : list)
        out += (out.size() > 1 ? "," : "") + quote(name);
    return out + ']';
}
// A media file from the PC helper (src/scraper.h, PC mode): written under a hidden
// name in the shared library and renamed into place when whole.
void prepare_scraped(MHD_Connection *c, Request &r)
{
    r.transfer = Transfer::scraped;
    r.job = arg(c, "id");
    r.task = unsigned(std::strtoul(arg(c, "task"), nullptr, 10));
    if (!content_length(c, r.expected) || r.expected > (uint64_t(256) << 20))
    {
        r.error = 413;
        r.message = "A media file is at most 256 MiB.";
        return;
    }
    if (!ps5_scraper::pc_target(r.job, r.task, r.temporary, r.destination) ||
        !r.writer.create(r.temporary))
    {
        r.temporary.clear();
        r.error = 409;
        r.message = "This download is no longer wanted.";
    }
}
std::string query(MHD_Connection *c, const char *key, const char *fallback)
{
    const char *value = arg(c, key);
    return *value ? value : fallback;
}
// The scraper's routes (src/scraper.h): they keep their own locks.
MHD_Result scraper_route(MHD_Connection *c, const std::string &url, const std::string &method,
                         Request &r)
{
    std::string why, id;
    auto options = [&]
    {
        ps5_scraper::Options o;
        o.mode = query(c, "mode", "");
        o.source = query(c, "source", "libretro");
        o.region = query(c, "region", "us");
        o.language = query(c, "language", "en");
        o.overwrite = std::strcmp(arg(c, "overwrite"), "1") == 0;
        const std::string kinds = arg(c, "kinds");
        size_t start = 0;
        while (start < kinds.size())
        {
            size_t end = kinds.find(',', start);
            if (end == std::string::npos)
                end = kinds.size();
            o.kinds.push_back(kinds.substr(start, end - start));
            start = end + 1;
        }
        return o;
    };
    if (method == "GET" && url == "/api/library")
        return respond(c, 200, ps5_scraper::library_json());
    if (method == "GET" && url == "/api/library/media")
    {
        const std::string file =
            ps5_scraper::media_file(arg(c, "system"), arg(c, "game"), arg(c, "kind"));
        if (file.empty())
            return error(c, 404, "No such media.");
        const std::string ext = file.substr(file.find_last_of('.') + 1);
        const char *type = ext == "png"                    ? "image/png"
                           : ext == "jpg" || ext == "jpeg" ? "image/jpeg"
                           : ext == "webp"                 ? "image/webp"
                           : ext == "gif"                  ? "image/gif"
                           : ext == "mp4"                  ? "video/mp4"
                           : ext == "webm"                 ? "video/webm"
                                                           : "application/octet-stream";
        return serve_file(c, file, type, false);
    }
    if (method == "GET" && url == "/api/scraper/settings")
        return respond(c, 200, ps5_scraper::settings_json());
    if (method == "POST" && url == "/api/scraper/settings")
        return ps5_scraper::save_settings(options())
                   ? respond(c, 200, ps5_scraper::settings_json())
                   : error(c, 500, "The settings could not be saved.");
    if (method == "GET" && url == "/api/scraper/job")
        return respond(c, 200, ps5_scraper::job_json(arg(c, "id")));
    if (method == "POST" && url == "/api/scraper/start")
    {
        // The body: one "system<TAB>path" a line, path empty for a whole system.
        std::vector<ps5_scraper::Selection> selection;
        size_t start = 0;
        while (start < r.body.size())
        {
            size_t end = r.body.find('\n', start);
            if (end == std::string::npos)
                end = r.body.size();
            const std::string line = r.body.substr(start, end - start);
            const size_t tab = line.find('\t');
            if (!line.empty())
                selection.push_back(
                    {line.substr(0, tab), tab == std::string::npos ? "" : line.substr(tab + 1)});
            start = end + 1;
        }
        if (!ps5_scraper::start(options(), selection, id, why))
            return error(c, 409, why.c_str());
        return respond(c, 201, ps5_scraper::job_json(id));
    }
    if (method == "POST" && url == "/api/scraper/cancel")
        return respond(c, 200,
                       std::string("{\"cancelled\":") +
                           (ps5_scraper::cancel(arg(c, "id")) ? "true" : "false") + '}');
    if (method == "POST" && url == "/api/scraper/resume")
        return ps5_scraper::resume(arg(c, "id"), why)
                   ? respond(c, 200, ps5_scraper::job_json(arg(c, "id")))
                   : error(c, 409, why.c_str());
    if (method == "POST" && url == "/api/scraper/resolve")
        return ps5_scraper::resolve(arg(c, "id"),
                                    unsigned(std::strtoul(arg(c, "item"), nullptr, 10)),
                                    arg(c, "action"), arg(c, "value"), why)
                   ? respond(c, 200, ps5_scraper::job_json(arg(c, "id")))
                   : error(c, 409, why.c_str());
    if (method == "GET" && url == "/api/scraper/pc/tasks")
        return respond(
            c, 200,
            ps5_scraper::pc_tasks(arg(c, "id"),
                                  unsigned(std::strtoul(query(c, "max", "8").c_str(), nullptr, 10)),
                                  std::strcmp(arg(c, "fresh"), "1") == 0));
    if (method == "POST" && url == "/api/scraper/pc/downloaded")
    {
        ps5_scraper::pc_downloaded(
            arg(c, "id"), unsigned(std::strtoul(arg(c, "task"), nullptr, 10)),
            std::strcmp(arg(c, "found"), "0") != 0, std::strtoull(arg(c, "bytes"), nullptr, 10));
        return respond(c, 200, "{\"noted\":true}");
    }
    if (r.transfer == Transfer::scraped)
    {
        const bool flushed = r.writer.flush();
        const uint64_t got = r.writer.written();
        const int fd = r.writer.release();
        bool ok = flushed && fd >= 0 && got == r.expected && r.received == r.expected;
        if (ok)
            ok = ps5_transfer::commit(fd, r.temporary, r.destination, true, true) ==
                 ps5_transfer::Commit::done;
        if (fd >= 0)
            close(fd);
        if (ok)
        {
            chmod(r.destination.c_str(), 0777);
            r.temporary.clear();
        }
        ps5_scraper::pc_delivered(r.job, r.task, ok, got);
        return ok ? respond(c, 201, "{\"stored\":true}")
                  : error(c, 507, "The media file could not be stored.");
    }
    return error(c, 404, "This page was not found.");
}
// The transfers' own routes: they run beside each other, outside state_lock.
MHD_Result transfer_route(MHD_Connection *c, const std::string &url, const std::string &method,
                          Request &r)
{
    if (r.transfer == Transfer::single)
    {
        if (r.received != r.expected)
            return error(c, 400, "The upload was incomplete. Try again.");
        const int fd = r.writer.release();
        if (fd < 0)
            return error(c, 507, "Could not finish writing the file. Check console storage.");
        const auto result = ps5_transfer::commit(fd, r.temporary, r.destination, r.replace, true);
        close(fd);
        if (result == ps5_transfer::Commit::exists)
            return error(c, 409, "A file with this name already exists. Rename your file first.");
        if (result != ps5_transfer::Commit::done)
            return error(c, 500, "Could not finish the upload.");
        r.temporary.clear();
        return respond(c, 201, "{\"uploaded\":true,\"bytes\":" + std::to_string(r.received) + '}');
    }
    if (r.transfer == Transfer::batch)
    {
        auto *state = r.batch.get();
        auto &b = state->upload;
        std::string failed = "[";
        for (const auto &f : b.failed)
            failed += (failed.size() > 1 ? "," : "") + std::string("{\"name\":") + quote(f.first) +
                      ",\"error\":" + quote(f.second) + '}';
        const bool whole = state->reader.finished() && r.received == r.expected;
        return respond(c, whole ? 200 : 400,
                       "{\"complete\":" + std::string(whole ? "true" : "false") + ",\"written\":" +
                           std::to_string(b.written) + ",\"skipped\":" + std::to_string(b.skipped) +
                           ",\"skippedNames\":" + names(b.skipped_names) + ",\"bytes\":" +
                           std::to_string(b.bytes) + ",\"failed\":" + failed + "],\"problem\":" +
                           quote(whole                      ? ""
                                 : *state->reader.problem() ? state->reader.problem()
                                                            : "The batch ended early.") +
                           '}');
    }
    if (r.transfer == Transfer::part)
    {
        if (r.received != r.expected || !r.writer.flush())
            return error(c, 507, "The part could not be written. Try it again.");
        uint64_t covered;
        {
            // Released here, not when the request is freed after its answer: a commit
            // sent the moment this answer arrives must find no writer left.
            std::lock_guard<std::mutex> guard(r.session->lock);
            r.session->add(r.offset, r.offset + r.received);
            r.session->touched = std::time(nullptr);
            covered = r.session->covered;
            if (--r.session->writers == 0 && r.session->closing && r.session->fd >= 0)
            {
                close(r.session->fd);
                r.session->fd = -1;
            }
        }
        r.session.reset();
        return respond(c, 200, "{\"covered\":" + std::to_string(covered) + '}');
    }
    if (method == "POST" && url == "/api/upload/session")
    {
        sessions.expire(std::time(nullptr), session_idle_seconds);
        std::string destination;
        char *end = nullptr;
        const char *text = arg(c, "size");
        errno = 0;
        const uint64_t size = std::strtoull(text, &end, 10);
        const bool size_ok = *text && !errno && !*end && size <= upload_limit;
        const bool replace = std::strcmp(arg(c, "existing"), "replace") == 0;
        if (!size_ok)
            return error(c, 413, "Files must be 64 GiB or smaller.");
        if (!content_parents(arg(c, "path"), destination))
            return error(c, 400, "Choose a valid content folder and filename.");
        if (!replace && exists(destination))
            return error(c, 409, "A file with this name already exists. Rename your file first.");
        struct statvfs storage{};
        if (storage_space(storage) && size > uint64_t(storage.f_bavail) * storage.f_frsize)
            return error(c, 507, "There is not enough free space on the console.");
        const std::string id = nonce();
        const char *why = "";
        auto session = sessions.open(
            id, destination.substr(0, destination.find_last_of('/') + 1) + ".upload-" + id,
            destination, size, replace, why);
        if (!session)
            return error(c, 507, why);
        return respond(c, 201,
                       "{\"id\":" + quote(id) + ",\"size\":" + std::to_string(size) +
                           ",\"partSize\":" + std::to_string(32u << 20) + '}');
    }
    if (method == "POST" && url == "/api/upload/commit")
    {
        auto session = sessions.find(arg(c, "id"));
        if (!session)
            return error(c, 404, "This upload is no longer open. Start it again.");
        {
            std::lock_guard<std::mutex> guard(session->lock);
            if (session->writers || session->covered != session->size)
                return respond(c, 409,
                               "{\"error\":\"Parts are missing.\",\"covered\":" +
                                   std::to_string(session->covered) + '}');
        }
        if (sessions.take(session->id) != session)
            return error(c, 409, "This upload was already finished.");
        const auto result = ps5_transfer::commit(session->fd, session->temporary,
                                                 session->destination, session->replace, true);
        close(session->fd);
        session->fd = -1;
        if (result != ps5_transfer::Commit::done)
        {
            unlink(session->temporary.c_str());
            return result == ps5_transfer::Commit::exists
                       ? error(c, 409,
                               "A file with this name already exists. Rename your file first.")
                       : error(c, 500, "Could not finish the upload.");
        }
        return respond(c, 201,
                       "{\"uploaded\":true,\"bytes\":" + std::to_string(session->size) + '}');
    }
    if (method == "DELETE" && url == "/api/upload/session")
    {
        auto session = sessions.take(arg(c, "id"));
        if (session)
            ps5_transfer::discard(*session);
        return respond(c, 200, "{\"cancelled\":" + std::string(session ? "true" : "false") + '}');
    }
    return error(c, 404, "This page was not found.");
}
bool is_transfer_route(const std::string &url)
{
    return url.rfind("/api/upload", 0) == 0;
}
bool is_scraper_route(const std::string &url)
{
    return url.rfind("/api/scraper", 0) == 0 || url.rfind("/api/library", 0) == 0;
}
std::string current_frontend()
{
    std::lock_guard<std::mutex> guard(frontend_lock);
    return quote(frontend_name);
}
MHD_Result route(MHD_Connection *c, const std::string &url, const std::string &method, Request &r)
{
    if (method == "GET" && url == "/api/status")
    {
        struct statvfs fs{};
        bool space_known = storage_space(fs);
        return respond(
            c, 200,
            "{\"name\":\"RetroArch\",\"frontend\":" + current_frontend() +
                ",\"port\":" + std::to_string(listen_port) + ",\"token\":" + quote(token) +
                ",\"uploadLimit\":" + std::to_string(upload_limit) + ",\"freeBytes\":" +
                (space_known ? std::to_string(uint64_t(fs.f_bavail) * fs.f_frsize) : "null") + '}');
    }
    if (method == "GET" && url == "/api/alerts")
        return respond(c, 200, bios_alerts());
    if (method == "GET" && url == "/api/update")
        return respond(c, 200, update_status());
    if (method == "POST" && url == "/api/update/download")
        return ps5_update::download(arg(c, "tag"))
                   ? respond(c, 202, update_status())
                   : error(c, 409,
                           "An update is already active, or the release could not be prepared.");
    if (method == "POST" && url == "/api/update/install")
        return ps5_update::request_install()
                   ? respond(c, 202, update_status())
                   : error(c, 409, "Download and verify an update before installing it.");
    if (method == "GET" && url == "/api/content")
        return list_content(c);
    if (method == "GET" && url == "/api/download")
        return download(c);
    if (url == "/api/core-metadata" && method == "GET")
    {
        const char *name = MHD_lookup_connection_value(c, MHD_GET_ARGUMENT_KIND, "core");
        const std::string core = name ? name : "";
        if (!valid_path(core) || core.find('/') != std::string::npos)
            return error(c, 400, "Choose a core profile.");
        const std::string empty = "{\"categories\":[],\"settings\":[]}";
        auto bundled =
            read_file(root_path + "/webui/core-metadata/" + core + ".json", 2 * 1024 * 1024);
        auto runtime = runtime_metadata(core, ".json");
        // Keep bundled options when a game's runtime table exposes only a subset.
        return respond(c, 200,
                       "{\"bundled\":" + (bundled.empty() ? empty : bundled) +
                           ",\"runtime\":" + (runtime.empty() ? empty : runtime) + '}');
    }
    if (url == "/api/config")
        return config_editor(c, method, r.body);
    if (method == "GET" && url == "/api/cores")
    {
        std::string out = "{\"cores\":[";
        for (const auto &core : core_profiles())
        {
            if (out.back() != '[')
                out += ',';
            out += quote(core);
        }
        return respond(c, 200, out + "]}");
    }
    if (method == "GET" && url == "/api/settings")
        return get_settings(c);
    if (method == "POST" && url == "/api/settings")
        return save_settings(c, r.body);
    if (method == "POST" && url == "/api/folder")
    {
        std::string path;
        const std::string relative = arg(c, "path");
        if (relative.empty() || !content_path(relative, path, true))
            return error(c, 400, "Enter a folder name inside content.");
        if (mkdir(path.c_str(), 0755))
            return error(c, errno == EEXIST ? 409 : 500,
                         "This folder already exists or could not be created.");
        return respond(c, 201, "{\"created\":true}");
    }
    if (method != "GET")
        return error(c, 405, "This action is not supported.");
    // Only shipped assets are reachable; no filesystem passthrough.
    const std::map<std::string, const char *> assets = {
        {"/", "text/html; charset=utf-8"},
        {"/index.html", "text/html; charset=utf-8"},
        {"/app.css", "text/css; charset=utf-8"},
        {"/app.js", "text/javascript; charset=utf-8"},
        {"/settings-guide.js", "text/javascript; charset=utf-8"},
        {"/version.json", "application/json"},
        {"/assets/mihawk.png", "image/png"},
        {"/assets/ui.woff2", "font/woff2"},
        {"/assets/retroarch.svg", "image/svg+xml"},
        {"/ps5-media-helper.py", "text/x-python; charset=utf-8"}};
    auto asset = assets.find(url);
    if (asset == assets.end())
        return error(c, 404, "This page was not found.");
    auto content =
        read_file(root_path + "/webui" + (url == "/" ? "/index.html" : url), 2 * 1024 * 1024);
    if (content.empty())
        return error(c, 404, "WebUI assets are missing. Reinstall the complete RetroArch package.");
    return respond(c, 200, content, asset->second);
}
MHD_Result handle(void *, MHD_Connection *c, const char *url, const char *method, const char *,
                  const char *data, size_t *size, void **context)
{
    if (!*context)
    {
        auto *r = new (std::nothrow) Request;
        if (!r)
            return MHD_NO;
        *context = r;
        if (!local_origin(c))
            return error(c, 403, "Open this page using the console IP address and port.");
        const bool write_request = std::strcmp(method, "GET") != 0;
        const char *provided = MHD_lookup_connection_value(c, MHD_HEADER_KIND, "X-RetroArch-Token");
        if (write_request && (!provided || token != provided))
            return error(c, 403, "Your session expired. Reload the page and try again.");
        if (std::strcmp(method, "PUT") == 0 && std::strcmp(url, "/api/upload") == 0)
            prepare_upload(c, *r);
        else if (std::strcmp(method, "PUT") == 0 && std::strcmp(url, "/api/upload/batch") == 0)
            prepare_batch(c, *r);
        else if (std::strcmp(method, "PUT") == 0 && std::strcmp(url, "/api/upload/part") == 0)
            prepare_part(c, *r);
        else if (std::strcmp(method, "PUT") == 0 && std::strcmp(url, "/api/scraper/pc/media") == 0)
            prepare_scraped(c, *r);
        if (std::strcmp(url, "/api/scraper/start") == 0)
            r->body_limit = 4 << 20; // one line a game chosen
        if (r->error)
            return error(c, r->error, r->message);
        if (r->transfer != Transfer::none)
        {
            ++transfers_in_flight;
            r->counted = true;
        }
        if (r->transfer == Transfer::none)
        {
            const char *length = MHD_lookup_connection_value(c, MHD_HEADER_KIND, "Content-Length");
            if (length && std::strtoull(length, nullptr, 10) > r->body_limit)
                return error(c, 413, "This request is too large.");
        }
        return MHD_YES;
    }
    auto &r = *static_cast<Request *>(*context);
    if (*size)
    {
        if (r.transfer != Transfer::none && !r.error)
        {
            if (*size > r.expected - r.received)
            {
                r.error = 413;
                r.message = "Upload exceeded its declared size.";
            }
            else if (r.transfer == Transfer::batch)
            {
                r.received += *size;
                if (!r.batch->reader.feed(data, *size))
                {
                    r.error = 400;
                    r.message = r.batch->reader.problem();
                }
            }
            else if (!r.writer.write(data, *size))
            {
                r.error = 507;
                r.message = "The console could not write the upload. Check free space.";
            }
            else
                r.received += *size;
        }
        else if (!r.error)
        {
            if (*size > r.body_limit - r.body.size())
            {
                r.error = 413;
                r.message = "This request is too large.";
            }
            else
                r.body.append(data, *size);
        }
        *size = 0;
        // MHD allows a response before receiving the body or after consuming it,
        // never during a body callback. Drain failed streams without buffering.
        return MHD_YES;
    }
    if (r.error && !(r.transfer == Transfer::batch && r.batch && r.error == 400))
        return error(c, r.error, r.message);
    if (r.transfer == Transfer::scraped || is_scraper_route(url))
        return scraper_route(c, url, method, r);
    if (r.transfer != Transfer::none || is_transfer_route(url))
        return transfer_route(c, url, method, r);
    std::lock_guard<std::mutex> guard(state_lock);
    return route(c, url, method, r);
}
constexpr unsigned transfer_threads = 4;
// Large socket buffers for each connection: a lane keeps the link full on its own.
void tune_connection(void *, MHD_Connection *c, void **, MHD_ConnectionNotificationCode code)
{
    if (code != MHD_CONNECTION_NOTIFY_STARTED)
        return;
    const auto *info = MHD_get_connection_info(c, MHD_CONNECTION_INFO_CONNECTION_FD);
    if (!info)
        return;
    int buffer = 4 << 20;
    setsockopt(info->connect_fd, SOL_SOCKET, SO_RCVBUF, &buffer, sizeof buffer);
    setsockopt(info->connect_fd, SOL_SOCKET, SO_SNDBUF, &buffer, sizeof buffer);
}
void completed(void *, MHD_Connection *, void **context, MHD_RequestTerminationCode)
{
    delete static_cast<Request *>(*context);
    *context = nullptr;
}
// Registration runs on the emulator thread. HTTP reads only the atomic snapshot.
void save_metadata(const char *name, std::string json, const Config &defaults)
{
    if (!name || root_path.empty() || !valid_path(name) || std::strchr(name, '/') ||
        json.size() > 2 * 1024 * 1024)
        return;
    const std::string folder = root_path + "/config/webui-metadata";
    mkdir(folder.c_str(), 0755);
    struct stat st{};
    if (content_stat(folder.c_str(), &st) || !S_ISDIR(st.st_mode))
        return;
    const auto tag = catalog_revision(name);
    json.insert(1, "\"catalogRevision\":" + quote(tag) + ',');
    // Both files carry their revision inside the atomic replacement, so an
    // interrupted write or an upgrade cannot validate a stale companion file.
    write_config(folder + '/' + name + ".opt", defaults, "# catalog-revision: " + tag + '\n');
    const std::string path = folder + '/' + name + ".json";
    const std::string temporary = path + ".tmp";
    int fd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    if (fd < 0)
        return;
    size_t offset = 0;
    while (offset < json.size())
    {
        ssize_t count = write(fd, json.data() + offset, json.size() - offset);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            break;
        offset += static_cast<size_t>(count);
    }
    bool ok = offset == json.size() && fsync(fd) == 0;
    close(fd);
    if (!ok || rename(temporary.c_str(), path.c_str()))
        unlink(temporary.c_str());
}
std::string metadata_text(const char *value)
{
    return quote(value ? value : "");
}
} // namespace
extern "C" void ps5_webui_core_options(const char *name, const retro_core_options_v2 *options)
{
    if (!options || !options->definitions)
        return;
    Config defaults;
    std::string json = "{\"categories\":[";
    if (options->categories)
        for (const auto *c = options->categories; c->key; ++c)
        {
            if (c != options->categories)
                json += ',';
            json += "{\"key\":" + metadata_text(c->key) + ",\"label\":" + metadata_text(c->desc) +
                    ",\"description\":" + metadata_text(c->info) + '}';
        }
    json += "],\"settings\":[";
    for (const auto *s = options->definitions; s->key; ++s)
    {
        if (s != options->definitions)
            json += ',';
        const char *value = s->default_value ? s->default_value : s->values[0].value;
        if (value && setting_key(s->key) && valid_setting_value(value, "text"))
            defaults[s->key] = value;
        json += "{\"key\":" + metadata_text(s->key) +
                ",\"label\":" + metadata_text(s->desc_categorized ? s->desc_categorized : s->desc) +
                ",\"description\":" +
                metadata_text(s->info_categorized ? s->info_categorized : s->info) +
                ",\"category\":" + metadata_text(s->category_key) +
                ",\"default\":" + metadata_text(value) + ",\"choices\":[";
        for (size_t i = 0; i < RETRO_NUM_CORE_OPTION_VALUES_MAX && s->values[i].value; ++i)
        {
            if (i)
                json += ',';
            json += '[' + metadata_text(s->values[i].value) + ',' +
                    metadata_text(s->values[i].label ? s->values[i].label : s->values[i].value) +
                    ']';
        }
        json += "]}";
    }
    save_metadata(name, json + "]}", defaults);
}
extern "C" void ps5_webui_core_variables(const char *name, const retro_variable *vars)
{
    if (!vars)
        return;
    Config defaults;
    std::string json = "{\"categories\":[],\"settings\":[";
    bool first = true;
    for (const auto *s = vars; s->key; ++s)
    {
        std::string value = s->value ? s->value : "";
        const size_t separator = value.find("; ");
        if (separator == std::string::npos)
            continue;
        if (!first)
            json += ',';
        first = false;
        json += "{\"key\":" + metadata_text(s->key) +
                ",\"label\":" + quote(value.substr(0, separator)) +
                ",\"description\":\"\",\"category\":\"\",\"choices\":[";
        size_t start = separator + 2;
        while (start <= value.size())
        {
            size_t end = value.find('|', start);
            if (end == std::string::npos)
                end = value.size();
            if (start != separator + 2)
                json += ',';
            const auto raw = value.substr(start, end - start);
            if (start == separator + 2 && setting_key(s->key) && valid_setting_value(raw, "text"))
                defaults[s->key] = raw;
            auto choice = quote(raw);
            json += '[' + choice + ',' + choice + ']';
            start = end + 1;
        }
        json += "]}";
    }
    save_metadata(name, json + "]}", defaults);
}
void ps5_webui_prepare(const char *root)
{
    root_path = root;
    apply_core_settings();
}
void ps5_webui_set_frontend(const char *frontend)
{
    std::lock_guard<std::mutex> guard(frontend_lock);
    frontend_name = frontend ? frontend : "";
}
unsigned ps5_webui_transfers()
{
    return transfers_in_flight.load();
}
bool ps5_webui_scraping()
{
    return ps5_scraper::busy();
}
unsigned ps5_webui_connections()
{
    if (!web_daemon)
        return 0;
    const MHD_DaemonInfo *info =
        MHD_get_daemon_info(web_daemon, MHD_DAEMON_INFO_CURRENT_CONNECTIONS);
    return info ? info->num_connections : 0;
}
bool ps5_webui_start(const char *root, unsigned short port, bool apply_settings)
{
    if (web_daemon)
        return true;
    root_path = root;
    ps5_update::initialize(root_path);
    // The scraper's jobs live with the server: one left running resumes now.
    ps5_scraper::configure(root_path, std::getenv("PS5_SCRAPER_LIBRETRO_BASE")
                                          ? std::getenv("PS5_SCRAPER_LIBRETRO_BASE")
                                          : "");
    if (apply_settings)
        apply_core_settings();
    listen_port = port;
    token = nonce();
    mkdir((root_path + "/content").c_str(), 0755);
    // A pool of threads, so parallel connections progress side by side (one thread
    // held a 4-connection upload to 15 MB/s, 2026-10-07); 512 KiB a connection, so a
    // body arrives in large pieces; up to 32 connections for the page's lanes.
    web_daemon = MHD_start_daemon(
        MHD_USE_INTERNAL_POLLING_THREAD | MHD_USE_ITC | MHD_USE_ERROR_LOG, port, nullptr, nullptr,
        handle, nullptr, MHD_OPTION_THREAD_POOL_SIZE, unsigned(transfer_threads),
        MHD_OPTION_CONNECTION_LIMIT, unsigned(32), MHD_OPTION_CONNECTION_TIMEOUT, unsigned(30),
        MHD_OPTION_CONNECTION_MEMORY_LIMIT, size_t(512 * 1024), MHD_OPTION_THREAD_STACK_SIZE,
        size_t(256 * 1024), MHD_OPTION_NOTIFY_COMPLETED, completed, nullptr,
        MHD_OPTION_NOTIFY_CONNECTION, tune_connection, nullptr, MHD_OPTION_END);
    std::fprintf(stderr, "webui: %s port=%u\n", web_daemon ? "listening" : "unavailable", port);
    return web_daemon != nullptr;
}
void ps5_webui_stop()
{
    if (web_daemon)
    {
        MHD_stop_daemon(web_daemon);
        web_daemon = nullptr;
        std::fprintf(stderr, "webui: stopped\n");
    }
    sessions.clear();
    ps5_scraper::shutdown();
    ps5_update::stop();
}
