/* Host harness for the same HTTP handlers shipped in the console title. */
#include "../src/webui_ps5.h"
#include <csignal>
#include <cstdlib>
#include <unistd.h>
static volatile sig_atomic_t stopped = 0;
static void stop(int)
{
    stopped = 1;
}
int main(int argc, char **argv)
{
    if (argc != 3)
        return 2;
    signal(SIGTERM, stop);
    signal(SIGINT, stop);
    if (!ps5_webui_start(argv[1], static_cast<unsigned short>(std::atoi(argv[2]))))
        return 1;
    while (!stopped)
        usleep(10000);
    ps5_webui_stop();
}
