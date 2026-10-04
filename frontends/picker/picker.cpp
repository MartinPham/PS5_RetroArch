/*
 * PS5 RetroArch - the frontend picker: RetroArch or EmulationStation, in Gloss.
 *
 * Copyright (C) 2026 Mihawk
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The title's pre-screen (docs/FRONTENDS.md). It is a program of PS5_VulkanTemplate's
 * UI module (BlackBearReloaded's kit, drawn with Vulkan), built into its own
 * executable, /app0/picker/picker.bin, by tools/build-picker.sh; eboot.bin starts it
 * through LoadExec when the title is launched from the home screen. It always wears
 * the kit's "gloss" theme (Gloss, Skeuomorphic), whatever theme the kit would pick.
 *
 * Two cards, each with a picture of the frontend it starts: RetroArch's XMB menu,
 * and EmulationStation's Alekfull NX system view, both captured on the console
 * (/app0/picker/previews/*.rgba, written by the build from assets/picker/). LEFT and
 * RIGHT move, CROSS starts the frontend, CIRCLE closes the title. Both frontends
 * play every game through RetroArch, with its cores, saves, shaders and settings.
 *
 * Choosing ends the program, so its device and display are released, and then
 * ps5_title_next (called by the title's main once the program is gone) restarts
 * the title as the frontend: eboot.bin with --ps5-mode=retroarch, or
 * /app0/es-de/es-de.bin.
 *
 * Armed by /app0/picker/picker-test.txt ("<frames> <retroarch|es-de|none>",
 * tools/run-title.sh --picker-test), the picker draws that many frames without the
 * pad, moves to the card named, saves its last frame as
 * /app0/picker/screenshots/picker.ppm, chooses it, and records what it chose in
 * /app0/picker/picker-test.jsonl. The file is removed when read: it arms one launch.
 */

#include "kit.hpp"

extern "C" int sceSystemServiceLoadExec(const char *path, const char *const *argv);
extern "C" int sceKernelUsleep(uint32_t microseconds);

namespace {

using hui::gfx::Color;
using hui::gfx::Rect;

constexpr int cardCount = 2;
constexpr const char *testPath = PS5_APP_ROOT "/picker-test.txt";
constexpr const char *testRecord = PS5_APP_ROOT "/picker-test.jsonl";
constexpr const char *screenshotPath = PS5_APP_ROOT "/screenshots/picker.ppm";

struct Frontend {
	const char *id;
	const char *title;
	const char *subtitle;
	const char *preview;
	const char *next;     // the executable that runs it
	const char *argument; // its one argument
	Color accent;
};

const Frontend frontends[cardCount] = {
	{ "retroarch", "RetroArch", "The XMB menu: every core, setting and shader", PS5_APP_ROOT "/previews/retroarch.rgba",
		"/app0/eboot.bin", "--ps5-mode=retroarch", Color::rgb(0x7c4dff) },
	{ "es-de", "EmulationStation", "ES-DE in the Alekfull NX theme; games start in RetroArch",
		PS5_APP_ROOT "/previews/es-de.rgba", "/app0/es-de/es-de.bin", "", Color::rgb(0x00b4d8) },
};

// What runs once the program has ended: -1 for nothing (the title closes)
int nextFrontend = -1;
std::string testLine;

const hui::ui::Theme &glossTheme()
{
	for (const hui::ui::Theme &theme : hui::ui::themes()) {
		if (std::strcmp(theme.id, "gloss") == 0) {
			return theme;
		}
	}
	return hui::ui::default_theme();
}

// A preview: "PS5RGBA1", the width and height (little-endian 32-bit), then the pixels
std::vector<std::uint8_t> readPreview(const char *path, int &width, int &height)
{
	std::vector<std::uint8_t> pixels;
	FILE *file = std::fopen(path, "rb");
	if (!file) {
		return pixels;
	}
	char magic[8];
	std::uint32_t size[2];
	if (std::fread(magic, 1, 8, file) == 8 && std::memcmp(magic, "PS5RGBA1", 8) == 0 &&
		std::fread(size, sizeof(size[0]), 2, file) == 2 && size[0] > 0 && size[1] > 0 && size[0] <= 4096 &&
		size[1] <= 4096) {
		pixels.resize((size_t)size[0] * size[1] * 4);
		if (std::fread(pixels.data(), 1, pixels.size(), file) == pixels.size()) {
			width = (int)size[0];
			height = (int)size[1];
		} else {
			pixels.clear();
		}
	}
	std::fclose(file);
	return pixels;
}

class VulkanExample : public ps5ui::KitExample
{
public:
	int focus{ 0 };
	int chosen{ -2 }; // -2: still choosing, -1: close, else the frontend
	hui::tween::Spring shown[cardCount];
	std::uint32_t previews[cardCount]{};
	float previewAspect[cardCount]{ 16.0f / 9.0f, 16.0f / 9.0f };
	const hui::ui::Theme &theme = glossTheme();
	hui::gfx::DrawList scene;
	hui::ui::Feedback feedback;
	float clock{ 0.0f };
	int testChoice{ -2 }; // armed: the frontend to choose, or -1 for none

	VulkanExample() : KitExample()
	{
		title = "Frontend picker";
		name = "picker";
		settings.overlay = false;
		ps5.ownOverlay = true;
		defaultClearColor = { { 0.0f, 0.0f, 0.0f, 1.0f } };
	}

	void readTest()
	{
		FILE *file = std::fopen(testPath, "r");
		if (!file) {
			return;
		}
		unsigned frames = 0;
		char choice[16] = {};
		const int fields = std::fscanf(file, "%u %15s", &frames, choice);
		std::fclose(file);
		std::remove(testPath);
		if (fields != 2 || frames < 10 || frames > 36000) {
			return;
		}
		testChoice = -1;
		for (int i = 0; i < cardCount; i++) {
			if (std::strcmp(choice, frontends[i].id) == 0) {
				testChoice = i;
			}
		}
		benchmark.active = true;
		ps5.frameBudget = frames;
		mkdir(PS5_APP_ROOT "/screenshots", 0777);
		ps5.screenshotPath = screenshotPath;
		char line[64];
		std::snprintf(line, sizeof(line), "\"frames\":%u,\"choice\":\"%s\"", frames, choice);
		testLine = line;
	}

	void prepare() override
	{
		VulkanExampleBase::prepare();
		readTest();
		prepareKit({ .music = false, .covers = false });
		for (int i = 0; i < cardCount; i++) {
			int width = 0, height = 0;
			const std::vector<std::uint8_t> pixels = readPreview(frontends[i].preview, width, height);
			if (!pixels.empty()) {
				previews[i] = kit.renderer.create_texture(width, height, pixels.data());
				previewAspect[i] = (float)width / (float)height;
			}
			shown[i].snap(i == focus ? 1.0f : 0.0f);
		}
		prepared = true;
	}

	void move(int to)
	{
		if (to == focus) {
			return;
		}
		feedback.play(hui::audio::Cue::focus, 1.0f, to == 0 ? -0.35f : 0.35f);
		focus = to;
	}

	void update(float dt)
	{
		feedback.clear();
		hui::InputFrame input = kit.input();
		if (ps5.frameBudget) {
			// A test run is the same whoever holds the pad: it moves to its card a third
			// of the way in, and chooses it on its last frame
			input = hui::InputFrame{};
			input.connected = true;
			if (testChoice >= 0 && ps5.framesDrawn == ps5.frameBudget / 3) {
				move(testChoice);
			}
			if (ps5.framesDrawn + 1 == ps5.frameBudget) {
				chosen = testChoice;
			}
		}
		clock += dt;
		if (input.nav == hui::Direction::left && focus > 0) {
			move(focus - 1);
		} else if (input.nav == hui::Direction::right && focus < cardCount - 1) {
			move(focus + 1);
		} else if (input.nav != hui::Direction::none && !input.nav_repeat) {
			feedback.play(hui::audio::Cue::error, 1.0f, 0.0f, 0.5f);
		}
		if (input.is_pressed(hui::Action::confirm)) {
			feedback.play(hui::audio::Cue::select);
			chosen = focus;
		} else if (input.is_pressed(hui::Action::back)) {
			chosen = -1;
		}
		for (int i = 0; i < cardCount; i++) {
			shown[i].target = i == focus ? 1.0f : 0.0f;
			shown[i].update(dt, theme.omega);
		}
		kit.play(feedback, theme.sounds);
		kit.tick(dt);
	}

	void compose()
	{
		const std::uint32_t glass = kit.renderer.glass_texture();
		scene.clear();
		hui::ui::Canvas canvas{ scene, kit.fonts, glass, clock };
		hui::ui::Painter painter(scene, kit.fonts, theme, glass);
		const hui::ui::ComponentStyle style{ theme };

		// The entrance: the heading fades in, then the cards rise one after the other
		const auto arrive = [this](float delay, float length) {
			return hui::tween::smoothstep(std::clamp((clock - delay) / length, 0.0f, 1.0f));
		};
		const float heading = arrive(0.0f, 0.5f);
		scene.push_opacity(heading);
		scene.push_transform(1.0f, 0.0f, 0.0f, 0.0f, (1.0f - heading) * 24.0f);
		painter.label("PS5 RETROARCH", 200.0f, 140.0f, 22.0f, painter.page_text_muted());
		painter.heading("Choose a frontend", 196.0f, 236.0f, 84.0f, painter.page_text());
		painter.body("Both play every game through RetroArch, with the same cores, saves, shaders and settings",
			200.0f, 292.0f, 26.0f, painter.page_text_muted());
		scene.pop_transform();
		scene.pop_opacity();

		hui::ui::CardLook look;
		look.art_aspect = 16.0f / 9.0f;
		look.title_size = 44.0f;
		look.subtitle_size = 24.0f;
		look.text_gap = 20.0f;
		look.glow = true;
		const float cardWidth = 700.0f;
		const float gap = 120.0f;
		for (int i = 0; i < cardCount; i++) {
			hui::ui::CardItem item;
			item.title = frontends[i].title;
			item.subtitle = frontends[i].subtitle;
			item.texture = previews[i];
			item.uv = hui::gfx::kFullUv;
			item.image_aspect = previewAspect[i];
			item.accent = frontends[i].accent;
			hui::ui::CardState state;
			state.focus = shown[i].value;
			const Rect card{ 200.0f + (cardWidth + gap) * (float)i, 360.0f, cardWidth,
				hui::ui::card_height(look, cardWidth) };
			const float rise = arrive(0.25f + 0.12f * (float)i, 0.55f);
			scene.push_opacity(rise);
			scene.push_transform(1.0f, 0.0f, 0.0f, 0.0f, (1.0f - rise) * 120.0f);
			hui::ui::draw_card(canvas, style, look, card, item, state);
			scene.pop_transform();
			scene.pop_opacity();
		}

		const float foot = arrive(0.75f, 0.4f);
		scene.push_opacity(foot);
		ps5ui::draw_hints(scene, kit.fonts, theme,
			{ { hui::ui::Button::dpad, "Move" }, { hui::ui::Button::cross, "Start" },
				{ hui::ui::Button::circle, "Close" } },
			960.0f, 1010.0f, 0);
		scene.pop_opacity();

		hui::gfx::BackdropSpec backdrop = theme.backdrop;
		backdrop.time = clock;
		kit.renderer.begin();
		kit.renderer.backdrop(backdrop);
		kit.renderer.glass();
		kit.renderer.draw(scene);
	}

	void render() override
	{
		if (!prepared) {
			return;
		}
		prepareFrame();
		update(std::min(frameTimer, 0.05f));
		compose();
		buildKitCommandBuffer();
		submitFrame();
		if (chosen != -2) {
			nextFrontend = chosen;
			quit = true;
		}
	}

	~VulkanExample() override
	{
		if (device) {
			vkDeviceWaitIdle(device);
		}
	}
};

} // namespace

// Called by the title's main once the program has ended and released its device
// (tools/build-picker.sh adds the call): the title restarts as the frontend chosen.
extern "C" void ps5_title_next(void)
{
	const Frontend *next = nextFrontend >= 0 && nextFrontend < cardCount ? &frontends[nextFrontend] : nullptr;
	if (!testLine.empty()) {
		if (FILE *record = std::fopen(testRecord, "a")) {
			std::fprintf(record, "{%s,\"next\":\"%s\"}\n", testLine.c_str(), next ? next->next : "");
			std::fclose(record);
			chmod(testRecord, 0666);
		}
	}
	if (!next) {
		return; // the title closes
	}
	say("picker: %s -> LoadExec(%s, \"%s\")", next->id, next->next, next->argument);
	const char *const argv[] = { next->argument, nullptr };
	const int result = sceSystemServiceLoadExec(next->next, argv);
	say("picker: LoadExec returned %d", result);
	for (;;) {
		sceKernelUsleep(100000);
	}
}

VULKAN_EXAMPLE_MAIN()
