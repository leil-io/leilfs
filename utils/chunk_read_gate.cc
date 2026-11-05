#include "common/platform.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <limits.h>
#include <unistd.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

// Test-only preload: hold a real read after hddRead has locked its chunk.
// The event loop can still answer discovery queries while that part is busy.
extern "C" ssize_t pread(int descriptor, void *buffer, size_t count, off_t offset) {
	using Read = ssize_t (*)(int, void *, size_t, off_t);
	static const auto original = reinterpret_cast<Read>(dlsym(RTLD_NEXT, "pread"));
	if (original == nullptr) { _exit(125); }

	const char *target = std::getenv("CHUNK_READ_GATE_TARGET");
	const char *armed = std::getenv("CHUNK_READ_GATE_ARMED");
	const char *held = std::getenv("CHUNK_READ_GATE_HELD");
	const char *release = std::getenv("CHUNK_READ_GATE_RELEASE");
	if (target != nullptr && armed != nullptr && held != nullptr && release != nullptr &&
	    access(armed, F_OK) == 0 && access(release, F_OK) != 0) {
		char link[64];
		char filename[PATH_MAX + 1];
		std::snprintf(link, sizeof(link), "/proc/self/fd/%d", descriptor);
		const auto length = readlink(link, filename, PATH_MAX);
		if (length > 0) {
			filename[length] = '\0';
			if (std::strcmp(filename, target) == 0) {
				const int marker = open(held, O_CREAT | O_WRONLY | O_TRUNC, 0644);
				if (marker < 0 || close(marker) != 0) { _exit(125); }
				const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(2);
				while (access(release, F_OK) != 0) {
					// Never release silently: that would hide a broken test premise.
					if (std::chrono::steady_clock::now() >= deadline) { _exit(124); }
					std::this_thread::sleep_for(std::chrono::milliseconds(10));
				}
			}
		}
	}
	return original(descriptor, buffer, count, offset);
}
