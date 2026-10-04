/* Which frontend a launch of eboot.bin starts (src/frontend_mode_ps5.cpp): the
 * decision for every launch, the mode argument as the console gives argv, and the
 * LoadExec each decision makes, against a LoadExec that replaces the process (a
 * thrown Replaced), refuses, or is accepted and ignored. argv[1] is a scratch
 * directory for the files the decision reads. */
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>
#include "../src/frontend_mode_ps5.cpp"

namespace
{
struct Replaced
{
};
enum class Exec
{
    replace,
    refuse,
    ignore
};
Exec exec_mode = Exec::replace;
int exec_calls = 0;
std::string exec_path, exec_argument;
std::vector<std::string> marks;

void touch(const std::string &path, bool present)
{
    if (!present)
    {
        std::remove(path.c_str());
        return;
    }
    std::FILE *file = std::fopen(path.c_str(), "wb");
    assert(file);
    std::fclose(file);
}

/* One launch: what it LoadExecs, or "retroarch" when it returns to run RetroArch. */
std::string launch(const ps5::frontend_mode::Paths &paths, std::vector<const char *> argv)
{
    argv.push_back(nullptr);
    exec_path.clear();
    try
    {
        ps5::frontend_mode::run(paths, static_cast<int>(argv.size() - 1),
                                const_cast<char **>(argv.data()), 0);
        return "retroarch";
    }
    catch (const Replaced &)
    {
        return exec_path;
    }
}
} // namespace

extern "C" int sceSystemServiceLoadExec(const char *path, const char *const *argv)
{
    exec_calls++;
    exec_path = path;
    exec_argument = argv && argv[0] ? argv[0] : "(none)";
    assert(argv && argv[0] && !argv[1]);
    if (exec_mode == Exec::replace)
        throw Replaced{};
    return exec_mode == Exec::refuse ? -1 : 0;
}

void ps5::debug::mark(const char *step) noexcept
{
    marks.emplace_back(step);
}
void ps5::debug::mark_value(const char *step, long long value) noexcept
{
    marks.emplace_back(std::string(step) + " " + std::to_string(value));
}

int main(int argc, char **argv)
{
    using ps5::frontend_mode::decide;
    using ps5::frontend_mode::Launch;
    using ps5::frontend_mode::Next;
    assert(argc == 2);
    const std::string dir = argv[1];

    /* The decision, for every launch. */
    for (unsigned state = 0; state < 64; state++)
    {
        for (const char *mode : {"", "retroarch", "es-de", "picker", "game"})
        {
            Launch launch;
            launch.mode = mode;
            launch.test_run = state & 1;
            launch.picker_test = state & 2;
            launch.picker_present = state & 4;
            launch.es_de_present = state & 8;
            const Next next = decide(launch);
            const std::string m = mode;
            if (m == "es-de")
                assert(next == (launch.es_de_present ? Next::es_de : Next::retroarch));
            else if (m == "picker")
                assert(next == (launch.picker_present ? Next::picker : Next::retroarch));
            else if (!m.empty())
                assert(next == Next::retroarch);
            else if (!launch.picker_present)
                assert(next == Next::retroarch);
            else
                assert(next ==
                       (launch.picker_test || !launch.test_run ? Next::picker : Next::retroarch));
        }
    }

    /* The mode argument, as the console gives argv: no program name, and an empty
     * argument from the home screen. */
    {
        char empty[] = "", mode[] = "--ps5-mode=retroarch", other[] = "--ps5-relaunch=2";
        char *home[] = {empty, nullptr};
        char *chosen[] = {mode, nullptr};
        char *both[] = {other, mode, nullptr};
        assert(ps5::frontend_mode::mode_argument(1, home).empty());
        assert(ps5::frontend_mode::mode_argument(1, chosen) == "retroarch");
        assert(ps5::frontend_mode::mode_argument(2, both) == "retroarch");
        assert(ps5::frontend_mode::mode_argument(0, nullptr).empty());
    }

    /* The launches, against files in the scratch directory. */
    const ps5::frontend_mode::Paths paths{dir + "/picker.bin", dir + "/es-de.bin",
                                          dir + "/test-run.txt", dir + "/picker-test.txt",
                                          dir + "/eboot.bin"};
    touch(paths.picker, true);
    touch(paths.es_de, true);
    touch(paths.test_run, false);
    touch(paths.picker_test, false);
    assert(launch(paths, {""}) == paths.picker && exec_argument.empty()); /* the home screen */
    assert(marks.back() ==
           "frontend: mode '', test run 0, picker test 0, picker 1, es-de 1 -> picker");
    assert(launch(paths, {"--ps5-mode=retroarch"}) == "retroarch");
    assert(launch(paths, {"--ps5-mode=es-de"}) == paths.es_de);
    touch(paths.test_run, true);
    assert(launch(paths, {""}) == "retroarch"); /* a test run stays RetroArch */
    touch(paths.picker_test, true);
    assert(launch(paths, {""}) == paths.picker); /* unless the picker test is armed */
    touch(paths.picker, false);
    assert(launch(paths, {""}) == "retroarch"); /* a title without the picker */
    touch(paths.es_de, false);
    assert(launch(paths, {"--ps5-mode=es-de"}) == "retroarch");

    /* A LoadExec refused, or accepted but not replacing the process, runs RetroArch. */
    touch(paths.picker, true);
    touch(paths.test_run, false);
    touch(paths.picker_test, false);
    for (Exec mode : {Exec::refuse, Exec::ignore})
    {
        exec_mode = mode;
        const int before = exec_calls;
        assert(launch(paths, {""}) == "retroarch" && exec_calls == before + 1 &&
               exec_path == paths.picker);
        assert(marks.back().rfind(
                   "frontend: LoadExec did not replace the process; RetroArch runs; result", 0) ==
               0);
    }
    /* After RetroArch quits: back to the picker only when the picker started it. */
    for (const char *mode : {"", "retroarch", "es-de", "picker"})
        for (bool present : {false, true})
            assert(ps5::frontend_mode::back_to_picker(mode, present) ==
                   (present && std::string(mode) == "retroarch"));
    const auto quit = [&](const char *mode)
    {
        exec_path.clear();
        try
        {
            ps5::frontend_mode::after_retroarch(paths, mode, 0);
            return std::string("closes");
        }
        catch (const Replaced &)
        {
            return exec_path;
        }
    };
    exec_mode = Exec::replace;
    assert(quit("retroarch") == paths.eboot && exec_argument.empty());
    assert(quit("") == "closes"); /* a test run's RetroArch closes the title */
    touch(paths.picker, false);
    assert(quit("retroarch") == "closes");
    touch(paths.picker, true);
    exec_mode = Exec::refuse;
    assert(quit("retroarch") == "closes" &&
           marks.back().rfind("frontend: LoadExec did not replace the process; the title closes",
                              0) == 0);
    std::puts("frontend_mode_ps5: decisions, mode argument, LoadExec targets, test runs and "
              "refused or ignored LoadExec PASS; back to the picker after RetroArch");
    return 0;
}
