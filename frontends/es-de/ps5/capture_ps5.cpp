/* PS5 RetroArch - pictures of what EmulationStation draws, for evidence and previews.
 *
 * Copyright (C) 2026 Mihawk
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Armed by /app0/es-de/capture-test.txt (tools/run-title.sh --frontend-capture):
 *
 *     <seconds>[,<seconds>...] <run id>
 *
 * ES-DE presents each frame with SDL_GL_SwapWindow, which the link wraps
 * (frontends/es-de/link.sh). When a frame is presented that many seconds after
 * the first, its back buffer is read and written as
 * /app0/es-de/capture-<run id>-<seconds>.ppm (binary RGB, top row first), and a
 * line is added to /app0/es-de/capture-test.jsonl with the frame's number, size
 * and the OpenGL error state. Unarmed, this costs one failed open, once.
 */
#include <SDL.h>

#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern "C" void __real_SDL_GL_SwapWindow(SDL_Window *window);

namespace
{
constexpr const char *arm_file = "/app0/es-de/capture-test.txt";
constexpr const char *record_file = "/app0/es-de/capture-test.jsonl";

struct Capture
{
    bool read = false;
    std::vector<unsigned> seconds;
    std::string run;
    size_t next = 0;
    unsigned long long frames = 0;
    std::chrono::steady_clock::time_point first;
};

Capture state;

bool valid_run(const std::string &run)
{
    if (run.empty() || run.size() > 64)
        return false;
    for (char c : run)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_'))
            return false;
    return true;
}

void read_arm()
{
    state.read = true;
    std::FILE *arm = std::fopen(arm_file, "r");
    if (!arm)
        return;
    char list[128] = {}, run[80] = {};
    const int fields = std::fscanf(arm, "%127s %79s", list, run);
    std::fclose(arm);
    if (fields != 2 || !valid_run(run))
        return;
    unsigned last = 0;
    for (char *token = std::strtok(list, ","); token; token = std::strtok(nullptr, ","))
    {
        char *end = nullptr;
        const unsigned long value = std::strtoul(token, &end, 10);
        if (!end || *end || value == 0 || value > 3600 || value <= last)
            return;
        state.seconds.push_back(static_cast<unsigned>(value));
        last = static_cast<unsigned>(value);
    }
    state.run = run;
}

void record(unsigned seconds, int width, int height, const std::string &file, unsigned error, bool written)
{
    if (std::FILE *out = std::fopen(record_file, "a"))
    {
        std::fprintf(out,
                     "{\"run\":\"%s\",\"seconds\":%u,\"frame\":%llu,\"width\":%d,\"height\":%d,"
                     "\"file\":\"%s\",\"gl_error\":%u,\"written\":%s}\n",
                     state.run.c_str(), seconds, state.frames, width, height, file.c_str(), error,
                     written ? "true" : "false");
        std::fclose(out);
    }
}

void capture(SDL_Window *window, unsigned seconds)
{
    int width = 0, height = 0;
    SDL_GL_GetDrawableSize(window, &width, &height);
    const std::string file = "/app0/es-de/capture-" + state.run + "-" + std::to_string(seconds) + ".ppm";
    if (width <= 0 || height <= 0 || width > 8192 || height > 8192)
    {
        record(seconds, width, height, file, 0, false);
        return;
    }
    while (glGetError() != GL_NO_ERROR)
    {
    }
    GLint read_framebuffer = 0, pack_alignment = 4;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_framebuffer);
    glGetIntegerv(GL_PACK_ALIGNMENT, &pack_alignment);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glReadBuffer(GL_BACK);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    const size_t row = static_cast<size_t>(width) * 3;
    std::vector<unsigned char> pixels(row * static_cast<size_t>(height));
    glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
    const unsigned error = glGetError();
    glPixelStorei(GL_PACK_ALIGNMENT, pack_alignment);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(read_framebuffer));

    bool written = false;
    if (std::FILE *out = std::fopen(file.c_str(), "wb"))
    {
        std::fprintf(out, "P6\n%d %d\n255\n", width, height);
        written = true;
        for (int y = height - 1; y >= 0 && written; y--)
            written = std::fwrite(pixels.data() + row * static_cast<size_t>(y), 1, row, out) == row;
        written = std::fclose(out) == 0 && written;
    }
    record(seconds, width, height, file, error, written);
}
} // namespace

extern "C" void __wrap_SDL_GL_SwapWindow(SDL_Window *window)
{
    if (!state.read)
    {
        read_arm();
        state.first = std::chrono::steady_clock::now();
    }
    state.frames++;
    if (state.next < state.seconds.size())
    {
        const auto elapsed = std::chrono::steady_clock::now() - state.first;
        const unsigned seconds = state.seconds[state.next];
        if (elapsed >= std::chrono::seconds(seconds))
        {
            capture(window, seconds);
            state.next++;
        }
    }
    __real_SDL_GL_SwapWindow(window);
}
