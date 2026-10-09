// bridge.cpp — browser entry point for Torch (see CMakeLists.txt).
//
// The worker writes the player's ROM and the extraction recipe (config.yml +
// yamls/) into the in-memory filesystem, then calls torch_extract_o2r.
// Return codes match port/android_torch_bridge.cpp:
//   0 ok, -2 ROM missing, 1/2 exception, 3 ROM not a supported dump (or the
//   recipe is missing), 4 archive not written.

#include "Companion.h"

#include <emscripten.h>

#include <atomic>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <string>

extern "C" EMSCRIPTEN_KEEPALIVE int torch_extract_o2r(const char *rom_path, const char *src_dir, const char *dst_dir)
{
	std::error_code ec;
	if (!std::filesystem::exists(rom_path, ec)) {
		std::fprintf(stderr, "torch: ROM missing: %s\n", rom_path);
		return -2;
	}
	try {
		auto *instance =
		    new Companion(std::filesystem::path(rom_path), ArchiveType::O2R, false, std::string(src_dir),
		                  std::string(dst_dir));
		Companion::Instance = instance;
		std::atomic<size_t> count{ 0 };
		instance->Init(ExportType::Binary, count);
		instance->Process(count); /* Init skips this under Emscripten */
		const std::string out = instance->GetOutputPath();
		Companion::Instance = nullptr;
		delete instance;
		if (out.empty()) {
			std::fprintf(stderr, "torch: ROM not recognized (not Super Smash Bros. US v1.0?)\n");
			return 3;
		}
		if (!std::filesystem::exists(out, ec)) {
			std::fprintf(stderr, "torch: %s was not written\n", out.c_str());
			return 4;
		}
		std::printf("torch: wrote %s (%zu assets)\n", out.c_str(), (size_t)count);
		return 0;
	} catch (const std::exception &e) {
		std::fprintf(stderr, "torch: %s\n", e.what());
		return 1;
	} catch (...) {
		std::fprintf(stderr, "torch: unknown error\n");
		return 2;
	}
}
