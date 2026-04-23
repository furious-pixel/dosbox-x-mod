#include "mod.h"

#include "control.h"
#include "dosbox_python.h"
#include "logging.h"
#include "mem.h"

#include <algorithm>
#include <cctype>
#include <vector>

namespace {

struct ModExecutableRuntime {
	ModExecutableConfig config = {};
	bool delta_ready = false;
	int64_t delta = 0;
};

struct ModRuntime {
	bool initialized = false;
	bool pending_scan = false;
	uint16_t pending_scan_handle = 0;
	size_t pending_scan_index = 0;
	std::vector<ModExecutableRuntime> executables = {};
};

ModRuntime g_mod = {};

static std::string uppercase_ascii_copy(std::string value)
{
	std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
		return static_cast<char>(std::toupper(ch));
	});
	return value;
}

static std::string basename_of(const std::string &path)
{
	const size_t pos = path.find_last_of("/\\");
	if (pos == std::string::npos)
		return path;
	return path.substr(pos + 1);
}

static uint32_t clamp_scan_end(uint64_t address)
{
	if (address > 0xffffffffull)
		return 0xffffffffu;
	return static_cast<uint32_t>(address);
}

static bool find_landmark_address(const ModExecutableConfig &config,
                                  uint32_t *found_address)
{
	if (config.landmark_string.empty())
		return false;

	const uint64_t total_bytes = static_cast<uint64_t>(MEM_TotalPages()) * MEM_PAGESIZE;
	const uint32_t max_address = clamp_scan_end(total_bytes);
	if (max_address == 0)
		return false;

	uint32_t scan_start = 0;
	uint32_t scan_end = max_address;
	if (config.has_scan_range) {
		scan_start = std::min(config.scan_start, max_address);
		scan_end = std::min(config.scan_end, max_address);
	}

	if (scan_start >= scan_end)
		return false;

	const size_t needle_size = config.landmark_string.size();
	if (needle_size == 0 || static_cast<uint64_t>(scan_start) + needle_size > scan_end)
		return false;

	for (uint32_t addr = scan_start;
	     static_cast<uint64_t>(addr) + needle_size <= scan_end;
	     ++addr) {
		if (mem_readb(addr) != static_cast<uint8_t>(config.landmark_string[0]))
			continue;

		bool matched = true;
		for (size_t i = 1; i < needle_size; ++i) {
			if (mem_readb(addr + static_cast<uint32_t>(i)) !=
			    static_cast<uint8_t>(config.landmark_string[i])) {
				matched = false;
				break;
			}
		}

		if (matched) {
			*found_address = addr;
			return true;
		}
	}

	return false;
}

static void run_pending_scan(ModExecutableRuntime &runtime)
{
	uint32_t found_address = 0;
	if (!find_landmark_address(runtime.config, &found_address)) {
		LOG_MSG("MOD ERROR: could not match %s landmark '%s'",
		        runtime.config.name.c_str(), runtime.config.landmark_string.c_str());
		return;
	}

	const int64_t previous_delta = runtime.delta;
	const bool had_previous_delta = runtime.delta_ready;
	const int64_t new_delta = static_cast<int64_t>(found_address) -
	                          static_cast<int64_t>(runtime.config.landmark_reloc);

	runtime.delta = new_delta;
	runtime.delta_ready = true;

	if (runtime.delta >= 0) {
		LOG_MSG("MOD: %s delta = 0x%08lX",
		        runtime.config.name.c_str(), static_cast<unsigned long>(runtime.delta));
	} else {
		const unsigned long magnitude =
		        static_cast<unsigned long>(-runtime.delta);
		LOG_MSG("MOD: %s delta = -0x%08lX",
		        runtime.config.name.c_str(), magnitude);
	}

	if (had_previous_delta && previous_delta != new_delta) {
		if (previous_delta >= 0 && new_delta >= 0) {
			LOG_MSG("MOD ERROR: %s delta changed from 0x%08lX to 0x%08lX",
			        runtime.config.name.c_str(),
			        static_cast<unsigned long>(previous_delta),
			        static_cast<unsigned long>(new_delta));
		} else if (previous_delta < 0 && new_delta < 0) {
			LOG_MSG("MOD ERROR: %s delta changed from -0x%08lX to -0x%08lX",
			        runtime.config.name.c_str(),
			        static_cast<unsigned long>(-previous_delta),
			        static_cast<unsigned long>(-new_delta));
		} else if (previous_delta < 0) {
			LOG_MSG("MOD ERROR: %s delta changed from -0x%08lX to 0x%08lX",
			        runtime.config.name.c_str(),
			        static_cast<unsigned long>(-previous_delta),
			        static_cast<unsigned long>(new_delta));
		} else {
			LOG_MSG("MOD ERROR: %s delta changed from 0x%08lX to -0x%08lX",
			        runtime.config.name.c_str(),
			        static_cast<unsigned long>(previous_delta),
			        static_cast<unsigned long>(-new_delta));
		}
	}
}

} // namespace

bool MOD_Init(const Config& config)
{
	g_mod = {};

	if (!DOSBoxPython_Init(config))
		return false;

	std::vector<ModExecutableConfig> configs;
	if (DOSBoxPython_LoadModInitConfigs(&configs)) {
		g_mod.executables.reserve(configs.size());
		for (size_t i = 0; i < configs.size(); ++i) {
			ModExecutableRuntime runtime = {};
			runtime.config = configs[i];
			g_mod.executables.push_back(runtime);
		}
	}

	g_mod.initialized = true;
	return true;
}

void MOD_Shutdown(void)
{
	g_mod = {};
	DOSBoxPython_Shutdown();
}

void MOD_OnOpenFile(const char *name, unsigned short handle)
{
	if (!g_mod.initialized || !name || g_mod.executables.empty())
		return;

	const std::string basename_upper = uppercase_ascii_copy(basename_of(name));
	for (size_t i = 0; i < g_mod.executables.size(); ++i) {
		ModExecutableRuntime &runtime = g_mod.executables[i];
		if (basename_upper != runtime.config.name_upper)
			continue;

		g_mod.pending_scan = true;
		g_mod.pending_scan_handle = handle;
		g_mod.pending_scan_index = i;
		LOG_MSG("MOD: armed %s on handle %u",
		        runtime.config.name.c_str(), static_cast<unsigned int>(handle));
		return;
	}
}

void MOD_OnCloseFile(unsigned short handle)
{
	if (!g_mod.initialized || !g_mod.pending_scan)
		return;

	if (handle != g_mod.pending_scan_handle)
		return;

	g_mod.pending_scan = false;
	g_mod.pending_scan_handle = 0;

	if (g_mod.pending_scan_index >= g_mod.executables.size())
		return;

	run_pending_scan(g_mod.executables[g_mod.pending_scan_index]);
}
