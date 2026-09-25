#include "dosbox_native_renderer.h"

#include "control.h"
#include "cross.h"
#include "dos_inc.h"
#include "../dos/drives.h"
#define MW2ER_ABI_NO_LINK
#include "mw2er_abi.h"
#undef MW2ER_ABI_NO_LINK
#include "logging.h"
#include "mem.h"
#include "mod.h"
#include "mixer.h"
#include "paging.h"

#if C_OPENGL && defined(C_SDL2)
#include <SDL.h>
#endif

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cctype>
#include <climits>
#include <cstdio>
#include <cstring>
#include <exception>
#include <string>

#if defined(WIN32) && !defined(HX_DOS)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dbghelp.h>
#endif

bool CodePageGuestToHostUTF8(char *destination, const char *source);

namespace {

struct NativeRendererState {
#if defined(WIN32) && !defined(HX_DOS)
	HMODULE dll = NULL;
#endif
	#if C_OPENGL && defined(C_SDL2)
    SDL_Window *context_window = nullptr;
    SDL_GLContext owned_context = nullptr;
#endif
    const Mw2erApi *api = NULL;
	const Mw2erRendererDescriptor *descriptor = NULL;
	bool enabled = false;
	bool init_started = false;
	bool mission = false;
	bool context_ready = false;
	uint64_t next_session_generation = 0;
	uint64_t next_mission_generation = 0;
	uint64_t session_generation = 0;
	uint64_t mission_generation = 0;
	uint64_t resource_generation = 0;
	uint64_t context_generation = 0;
	int64_t delta = 0;
	double counter_to_seconds = 0.0;
	uint32_t render_hook_reloc = 0;
	Mw2erViewport viewport = {};
	std::string mods_dir = {};
	std::string source_name = {};
	std::string opened_path = {};
};

NativeRendererState g_native = {};

#if defined(WIN32) && !defined(HX_DOS)
struct NativeCrashDiagnostics {
	std::atomic<const char *> operation = {"idle"};
	std::atomic<uint64_t> frame = {0};
	std::atomic<uint64_t> session = {0};
	std::atomic<uint64_t> mission = {0};
	std::atomic<uint64_t> context = {0};
	std::atomic<int> depth = {0};
	std::atomic<bool> active = {false};
	char text_path[MAX_PATH] = {};
	char dump_path[MAX_PATH] = {};
	bool installed = false;
	LPTOP_LEVEL_EXCEPTION_FILTER previous = NULL;
};

NativeCrashDiagnostics g_native_crash = {};

static LONG WINAPI native_renderer_crash_filter(EXCEPTION_POINTERS *info)
{
	if (!g_native_crash.active.load(std::memory_order_relaxed))
		return g_native_crash.previous &&
		                       g_native_crash.previous != native_renderer_crash_filter
		             ? g_native_crash.previous(info)
		             : EXCEPTION_CONTINUE_SEARCH;

	const DWORD code = info && info->ExceptionRecord
	                           ? info->ExceptionRecord->ExceptionCode
	                           : 0;
	void *address = info && info->ExceptionRecord
	                        ? info->ExceptionRecord->ExceptionAddress
	                        : NULL;
	MEMORY_BASIC_INFORMATION memory = {};
	const bool in_renderer = address &&
	                         VirtualQuery(address, &memory, sizeof(memory)) &&
	                         memory.AllocationBase == g_native.dll;
	const uintptr_t offset = in_renderer
	                               ? reinterpret_cast<uintptr_t>(address) -
	                                         reinterpret_cast<uintptr_t>(g_native.dll)
	                               : 0;
	char message[1024] = {};
	const int length = snprintf(
	        message,
	        sizeof(message),
	        "native renderer crash\r\n"
	        "exception = 0x%08lX\r\n"
	        "address = %p\r\n"
	        "renderer_module = %s\r\n"
	        "renderer_offset = 0x%llX\r\n"
	        "operation = %s\r\n"
	        "frame = %llu\r\n"
	        "session = %llu\r\n"
	        "mission = %llu\r\n"
	        "context = %llu\r\n",
	        static_cast<unsigned long>(code),
	        address,
	        in_renderer ? "yes" : "no",
	        static_cast<unsigned long long>(offset),
	        g_native_crash.operation.load(std::memory_order_relaxed),
	        static_cast<unsigned long long>(
	                g_native_crash.frame.load(std::memory_order_relaxed)),
	        static_cast<unsigned long long>(
	                g_native_crash.session.load(std::memory_order_relaxed)),
	        static_cast<unsigned long long>(
	                g_native_crash.mission.load(std::memory_order_relaxed)),
	        static_cast<unsigned long long>(
	                g_native_crash.context.load(std::memory_order_relaxed)));
	HANDLE text_file = CreateFileA(g_native_crash.text_path,
	                               GENERIC_WRITE,
	                               FILE_SHARE_READ,
	                               NULL,
	                               CREATE_ALWAYS,
	                               FILE_ATTRIBUTE_NORMAL,
	                               NULL);
	if (text_file != INVALID_HANDLE_VALUE) {
		DWORD written = 0;
		const DWORD message_bytes = static_cast<DWORD>(
		        length > 0
		                ? std::min(static_cast<size_t>(length), sizeof(message) - 1u)
		                : 0u);
		WriteFile(text_file,
		          message,
		          message_bytes,
		          &written,
		          NULL);
		FlushFileBuffers(text_file);
		CloseHandle(text_file);
	}

	HMODULE dbghelp = LoadLibraryA("dbghelp.dll");
	if (dbghelp) {
		using MiniDumpWriteDumpFn = BOOL(WINAPI *)(
		        HANDLE,
		        DWORD,
		        HANDLE,
		        MINIDUMP_TYPE,
		        const MINIDUMP_EXCEPTION_INFORMATION *,
		        const MINIDUMP_USER_STREAM_INFORMATION *,
		        const MINIDUMP_CALLBACK_INFORMATION *);
		const auto write_dump = reinterpret_cast<MiniDumpWriteDumpFn>(
		        GetProcAddress(dbghelp, "MiniDumpWriteDump"));
		HANDLE dump_file = CreateFileA(g_native_crash.dump_path,
		                               GENERIC_WRITE,
		                               FILE_SHARE_READ,
		                               NULL,
		                               CREATE_ALWAYS,
		                               FILE_ATTRIBUTE_NORMAL,
		                               NULL);
		if (write_dump && dump_file != INVALID_HANDLE_VALUE) {
			MINIDUMP_EXCEPTION_INFORMATION exception = {};
			exception.ThreadId = GetCurrentThreadId();
			exception.ExceptionPointers = info;
			exception.ClientPointers = FALSE;
			write_dump(GetCurrentProcess(),
			           GetCurrentProcessId(),
			           dump_file,
			           MiniDumpNormal,
			           &exception,
			           NULL,
			           NULL);
		}
		if (dump_file != INVALID_HANDLE_VALUE)
			CloseHandle(dump_file);
		FreeLibrary(dbghelp);
	}

	return g_native_crash.previous &&
	                       g_native_crash.previous != native_renderer_crash_filter
	             ? g_native_crash.previous(info)
	             : EXCEPTION_CONTINUE_SEARCH;
}

static void install_native_crash_diagnostics(const std::string& mods_dir)
{
	const int text_length = snprintf(g_native_crash.text_path,
	                                 sizeof(g_native_crash.text_path),
	                                 "%s\\mw2renderer-crash.txt",
	                                 mods_dir.c_str());
	const int dump_length = snprintf(g_native_crash.dump_path,
	                                 sizeof(g_native_crash.dump_path),
	                                 "%s\\mw2renderer-crash.dmp",
	                                 mods_dir.c_str());
	if (text_length <= 0 ||
	    text_length >= static_cast<int>(sizeof(g_native_crash.text_path)) ||
	    dump_length <= 0 ||
	    dump_length >= static_cast<int>(sizeof(g_native_crash.dump_path))) {
		g_native_crash.text_path[0] = '\0';
		g_native_crash.dump_path[0] = '\0';
		return;
	}
	if (!g_native_crash.installed) {
		g_native_crash.previous =
		        SetUnhandledExceptionFilter(native_renderer_crash_filter);
		g_native_crash.installed = true;
	}
}

struct NativeCallScope {
	NativeCallScope(const char *operation, const uint64_t frame = 0)
	{
		g_native_crash.operation.store(operation, std::memory_order_relaxed);
		g_native_crash.frame.store(frame, std::memory_order_relaxed);
		g_native_crash.session.store(g_native.session_generation,
		                             std::memory_order_relaxed);
		g_native_crash.mission.store(g_native.mission_generation,
		                             std::memory_order_relaxed);
		g_native_crash.context.store(g_native.context_generation,
		                             std::memory_order_relaxed);
		if (g_native_crash.depth.fetch_add(1, std::memory_order_relaxed) == 0)
			g_native_crash.active.store(g_native_crash.text_path[0] != '\0',
			                            std::memory_order_relaxed);
	}
	~NativeCallScope()
	{
		g_native_crash.operation.store("idle", std::memory_order_relaxed);
		g_native_crash.frame.store(0, std::memory_order_relaxed);
		if (g_native_crash.depth.fetch_sub(1, std::memory_order_relaxed) == 1)
			g_native_crash.active.store(false, std::memory_order_relaxed);
	}
	void set_operation(const char *operation)
	{
		g_native_crash.operation.store(operation, std::memory_order_relaxed);
	}
};
#else
struct NativeCallScope {
	NativeCallScope(const char *, uint64_t = 0) {}
	void set_operation(const char *) {}
};
#endif

static std::string uppercase_ascii_copy(const char *text)
{
	std::string result = text ? text : "";
	std::transform(result.begin(), result.end(), result.begin(),
	               [](const unsigned char c) {
		               return static_cast<char>(std::toupper(c));
	               });
	return result;
}

static std::string absolute_path(const std::string& path)
{
#if defined(WIN32) && !defined(HX_DOS)
	char buffer[MAX_PATH] = {};
	const DWORD length = GetFullPathNameA(path.c_str(), MAX_PATH, buffer, NULL);
	if (length > 0 && length < MAX_PATH)
		return std::string(buffer, length);
#endif
	return path;
}

static std::string join_path(const std::string& base, const char *leaf)
{
	if (base.empty())
		return leaf ? leaf : "";
	const char last = base.back();
	return base + ((last == '/' || last == '\\') ? "" : "\\") + leaf;
}

static std::string basename_of(const std::string& path)
{
	const size_t separator = path.find_last_of("/\\");
	return separator == std::string::npos ? path : path.substr(separator + 1u);
}

static void native_log(const char *message)
{
	if (message)
		LOG_MSG("NATIVE RENDERER: %s", message);
}

static void *native_get_gl_proc(const char *name)
{
#if C_OPENGL && defined(C_SDL2)
	if (!name || SDL_GL_GetCurrentContext() == NULL)
		return NULL;
	return SDL_GL_GetProcAddress(name);
#else
	(void)name;
	return NULL;
#endif
}

static const char *last_error(void)
{
	if (!g_native.api || !g_native.api->last_error)
		return "unknown error";
	const char *message = g_native.api->last_error();
	return message && message[0] ? message : "unknown error";
}

static void release_renderer_policy()
{
	MOD_SetSceneRasterSuppression(false);
	MOD_SetFramePacingSuspended(false);
	MOD_SetFramePacingContinuousPresentation(false);
}

static void disable_renderer(const char *operation, const int32_t result)
{
	release_renderer_policy();
	LOG_MSG("NATIVE RENDERER ERROR: %s failed (%d): %s; disabling renderer",
	        operation ? operation : "operation",
	        static_cast<int>(result),
	        last_error());
	if (g_native.mission && g_native.api) {
		if (g_native.api->abi_version >= MW2ER_ABI_VERSION) {
			g_native.api->end_mission(g_native.mission_generation);
			g_native.api->end_session(g_native.session_generation);
		} else if (g_native.api->mission_end) {
			g_native.api->mission_end();
		}
	}
	g_native.mission = false;
	g_native.session_generation = 0;
	g_native.mission_generation = 0;
	g_native.resource_generation = 0;
	g_native.delta = 0;
	g_native.enabled = false;
}

static bool call_ok(const char *operation, const int32_t result)
{
	if (result == MW2ER_OK)
		return true;
	disable_renderer(operation, result);
	return false;
}

static bool frame_call_ok(const char *operation, const int32_t result)
{
	if (result == MW2ER_OK)
		return true;
	if (result == MW2ER_ERR_NOT_READY)
		return false;
	disable_renderer(operation, result);
	return false;
}

static bool valid_api(const Mw2erApi *api)
{
	if (!api || (api->abi_version != MW2ER_ABI_VERSION_V2 &&
	             api->abi_version != MW2ER_ABI_VERSION))
		return false;
	const size_t v2_size = offsetof(Mw2erApi, begin_session);
	if (api->struct_size < v2_size || !api->descriptor || !api->init ||
	    !api->shutdown || !api->last_error || !api->on_gl_context ||
	    !api->on_gl_context_lost || !api->mission_begin || !api->mission_end ||
	    !api->bind_frame || !api->render_scene || !api->render_hud ||
	    !api->publish || !api->composite || !api->published_scene ||
	    !api->last_cpu_timing)
		return false;
	if (api->abi_version == MW2ER_ABI_VERSION_V2)
		return true;
	const size_t v3_size = offsetof(Mw2erApi, resources_pending);
	return api->struct_size >= v3_size && api->begin_session &&
	       api->end_session && api->begin_mission && api->end_mission &&
	       api->capture && api->seal_frame && api->render_frame &&
	       api->publish_frame && api->composite_frame;
}

static bool has_resource_service(void)
{
	return g_native.api && g_native.api->abi_version >= MW2ER_ABI_VERSION &&
	       g_native.api->struct_size >= sizeof(Mw2erApi) &&
	       g_native.api->resources_pending && g_native.api->service_resources;
}

static uint32_t resource_type_reloc(const uint32_t type)
{
	if (type == MW2ER_RESOURCE_CEL)
		return 0x000AE9C4u;
	if (type == MW2ER_RESOURCE_POLY)
		return 0x000AE9E8u;
	return 0;
}

struct HostResourceContext {
	uint32_t acquire_context;
	uint32_t cel_type;
	uint32_t poly_type;
    std::exception_ptr interrupted;
};

static bool call_resource_guest(HostResourceContext *context, uint32_t address,
                                const ModGuestCallRegisters &input,
                                ModGuestCallRegisters *output) noexcept
{
    if (!context || context->interrupted) return false;
    try {
        return MOD_CallRelocFunction(address, input, output);
    } catch (...) {
        // Keep host exit/reset exceptions on the host side of the C ABI.
        context->interrupted = std::current_exception();
        return false;
    }
}

static uint32_t resource_type_context(const HostResourceContext& context,
                                      const uint32_t type)
{
	if (type == MW2ER_RESOURCE_CEL)
		return context.cel_type;
	if (type == MW2ER_RESOURCE_POLY)
		return context.poly_type;
	return 0;
}

static bool resource_type_ready(uint32_t type)
{
	return type != 0 && type != 0xFFFFFFFFu;
}

static int32_t acquire_resource(void *user,
                                const Mw2erResourceKey *key,
                                uint32_t *payload_runtime_address)
{
	if (!key || key->struct_size < sizeof(*key) || key->reserved != 0 ||
	    !payload_runtime_address ||
	    key->resource_generation != g_native.resource_generation)
		return MW2ER_ERR_INVALID_ARGUMENT;
	HostResourceContext *context =
	        static_cast<HostResourceContext *>(user);
	const uint32_t type_context = context
	                                      ? resource_type_context(*context, key->type)
	                                      : 0;
	// The acquire context is an opaque guest-call argument; zero is valid here.
	if (!context || !resource_type_ready(type_context))
		return MW2ER_ERR_NOT_READY;
	ModGuestCallRegisters input = {};
	ModGuestCallRegisters output = {};
	input.eax = context->acquire_context;
	input.ebx = type_context;
	input.edx = key->resource_id;
	if (!call_resource_guest(context, 0x0004B080u, input, &output))
		return MW2ER_ERR_GENERIC;
	*payload_runtime_address = output.eax;
	return MW2ER_OK;
}

static int32_t release_resource(void *user, const Mw2erResourceKey *key)
{
	if (!key || key->struct_size < sizeof(*key) || key->reserved != 0 ||
	    key->resource_generation != g_native.resource_generation)
		return MW2ER_ERR_INVALID_ARGUMENT;
	HostResourceContext *context =
	        static_cast<HostResourceContext *>(user);
	const uint32_t type_context = context
	                                      ? resource_type_context(*context, key->type)
	                                      : 0;
	if (!resource_type_ready(type_context))
		return MW2ER_ERR_NOT_READY;
	ModGuestCallRegisters input = {};
	ModGuestCallRegisters output = {};
	input.eax = key->resource_id;
	input.edx = type_context;
	return call_resource_guest(context, 0x0004B020u, input, &output)
	             ? MW2ER_OK
	             : MW2ER_ERR_GENERIC;
}

static bool render_hook(const Mw2erRendererDescriptor *descriptor,
                         const uint32_t abi_version,
                         uint32_t *reloc_eip)
{
	if (!descriptor || !reloc_eip)
		return false;
	*reloc_eip = 0;
	const size_t v2_size = offsetof(Mw2erRendererDescriptor, binding_count);
	if (descriptor->struct_size < v2_size || !descriptor->name ||
	    !descriptor->executable_name || !descriptor->name[0] ||
	    !descriptor->executable_name[0])
		return false;
	if (abi_version == MW2ER_ABI_VERSION_V2) {
		*reloc_eip = descriptor->render_hook_reloc;
		return *reloc_eip != 0;
	}
	if (descriptor->struct_size < sizeof(Mw2erRendererDescriptor) ||
	    descriptor->reserved != 0 || descriptor->binding_count == 0 ||
	    !descriptor->bindings)
		return false;
	if (descriptor->observed_file_name &&
	    (!descriptor->observed_file_name[0] ||
	     strchr(descriptor->observed_file_name, ':') ||
	     strchr(descriptor->observed_file_name, '/') ||
	     strchr(descriptor->observed_file_name, '\\')))
		return false;
	for (uint32_t i = 0; i < descriptor->binding_count; ++i) {
		const Mw2erHookBinding& binding = descriptor->bindings[i];
		if (binding.struct_size < sizeof(binding) || binding.event == 0 ||
		    binding.reloc_eip == 0 ||
		    (binding.kind != MW2ER_HOOK_CAPTURE &&
		     binding.kind != MW2ER_HOOK_RENDER &&
		     binding.kind != MW2ER_HOOK_COMPOSITOR))
			return false;
		if (binding.kind == MW2ER_HOOK_RENDER && *reloc_eip != 0)
			return false;
		if (binding.kind == MW2ER_HOOK_RENDER)
			*reloc_eip = binding.reloc_eip;
	}
	return *reloc_eip != 0;
}

static Mw2erViewport make_viewport(const ModOpenGLState& state)
{
	Mw2erViewport viewport = {};
	viewport.struct_size = sizeof(viewport);
	viewport.mod_x = static_cast<int32_t>(state.mod_viewport_x);
	viewport.mod_y = static_cast<int32_t>(state.mod_viewport_y);
	viewport.mod_w = static_cast<int32_t>(state.mod_viewport_w);
	viewport.mod_h = static_cast<int32_t>(state.mod_viewport_h);
	viewport.backbuffer_w = static_cast<int32_t>(state.backbuffer_width);
	viewport.backbuffer_h = static_cast<int32_t>(state.backbuffer_height);
	viewport.backbuffer_fbo = state.backbuffer_framebuffer;
	viewport.context_generation = state.context_generation;
	viewport.view_mode = static_cast<int32_t>(state.view_mode);
	return viewport;
}

static bool make_memory_view(const int64_t delta, Mw2erMemoryView *view)
{
	if (!view || !GetMemBase() || PAGING_Enabled()) {
		LOG_MSG("NATIVE RENDERER ERROR: zero-copy guest view requires paging off");
		return false;
	}
	if (delta < 0 || static_cast<uint64_t>(delta) > UINT32_MAX) {
		LOG_MSG("NATIVE RENDERER ERROR: relocation delta is outside ABI range");
		return false;
	}
	const uint64_t bytes = static_cast<uint64_t>(MEM_TotalPages()) * 4096u;
	if (bytes == 0 || bytes > UINT32_MAX) {
		LOG_MSG("NATIVE RENDERER ERROR: guest RAM size is outside ABI range");
		return false;
	}
	memset(view, 0, sizeof(*view));
	view->struct_size = sizeof(*view);
	view->bytes = GetMemBase();
	view->size = static_cast<uint32_t>(bytes);
	view->runtime_base = 0;
	view->delta = static_cast<uint32_t>(delta);
	return true;
}

} // namespace

bool DOSBoxNativeRenderer_Init(const Config& config)
{
	DOSBoxNativeRenderer_Shutdown();
#if !defined(WIN32) || defined(HX_DOS) || !C_OPENGL || !defined(C_SDL2)
	(void)config;
	return true;
#else
	if (config.opt_moddir.empty())
		return true;

	g_native.mods_dir = absolute_path(config.opt_moddir);
	install_native_crash_diagnostics(g_native.mods_dir);
	const std::string dll_path = join_path(g_native.mods_dir, "mw2renderer.dll");
	g_native.source_name = basename_of(dll_path);
	const DWORD attributes = GetFileAttributesA(dll_path.c_str());
	if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY))
		return true;

	g_native.dll = LoadLibraryA(dll_path.c_str());
	if (!g_native.dll) {
		LOG_MSG("NATIVE RENDERER ERROR: LoadLibrary failed for %s (error %lu)",
		        dll_path.c_str(), static_cast<unsigned long>(GetLastError()));
		return true;
	}
	/* The filter remains process-wide but writes artifacts only while an
	 * ABI call is on the stack. It records the active ABI operation and
	 * then permits normal crash termination; it never attempts recovery. */
	NativeCallScope crash_scope("load_api");
	const Mw2erGetApiFn get_api = reinterpret_cast<Mw2erGetApiFn>(
	        GetProcAddress(g_native.dll, "mw2er_get_api"));
	if (!get_api) {
		LOG_MSG("NATIVE RENDERER ERROR: mw2er_get_api is missing from %s",
		        dll_path.c_str());
		DOSBoxNativeRenderer_Shutdown();
		return true;
	}
	crash_scope.set_operation("get_api");
	const Mw2erApi *candidate = get_api();
	if (!valid_api(candidate)) {
		LOG_MSG("NATIVE RENDERER ERROR: unsupported or incomplete renderer ABI");
		DOSBoxNativeRenderer_Shutdown();
		return true;
	}
	g_native.api = candidate;
#if C_OPENGL && defined(C_SDL2)
	const uint64_t frequency = SDL_GetPerformanceFrequency();
	g_native.counter_to_seconds = frequency
	                                      ? 1.0 / static_cast<double>(frequency)
	                                      : 0.0;
#endif
	crash_scope.set_operation("descriptor");
	g_native.descriptor = g_native.api->descriptor();
	if (!render_hook(g_native.descriptor,
	                  g_native.api->abi_version,
	                  &g_native.render_hook_reloc)) {
		LOG_MSG("NATIVE RENDERER ERROR: invalid renderer descriptor");
		DOSBoxNativeRenderer_Shutdown();
		return true;
	}

	Mw2erInit init = {};
	init.struct_size = sizeof(init);
	init.mod_dir = g_native.mods_dir.c_str();
	init.log = native_log;
	init.get_gl_proc_address = native_get_gl_proc;
	crash_scope.set_operation("init");
	g_native.init_started = true;
	if (g_native.api->init(&init) != MW2ER_OK) {
		LOG_MSG("NATIVE RENDERER ERROR: init failed: %s", last_error());
		DOSBoxNativeRenderer_Shutdown();
		return true;
	}
	g_native.enabled = true;
	LOG_MSG("NATIVE RENDERER: loaded %s for %s hook 0x%08lX",
	        g_native.descriptor->name ? g_native.descriptor->name : "unnamed",
	        g_native.descriptor->executable_name,
	        static_cast<unsigned long>(g_native.render_hook_reloc));
	return true;
#endif
}

void DOSBoxNativeRenderer_Shutdown(void)
{
	release_renderer_policy();
	NativeCallScope crash_scope("shutdown");
	if (g_native.api && g_native.init_started) {
		if (g_native.mission) {
			if (g_native.api->abi_version >= MW2ER_ABI_VERSION) {
				crash_scope.set_operation("end_mission");
				g_native.api->end_mission(g_native.mission_generation);
			} else if (g_native.api->mission_end) {
				crash_scope.set_operation("mission_end");
				g_native.api->mission_end();
			}
		}
		if (g_native.session_generation != 0 &&
		    g_native.api->abi_version >= MW2ER_ABI_VERSION) {
			crash_scope.set_operation("end_session");
			g_native.api->end_session(g_native.session_generation);
		}
		DOSBoxNativeRenderer_NotifyOpenGLContextLost();
		if (g_native.api->shutdown) {
			crash_scope.set_operation("shutdown");
			g_native.api->shutdown();
		}
	}
#if defined(WIN32) && !defined(HX_DOS)
	if (g_native.dll) {
		crash_scope.set_operation("unload");
		FreeLibrary(g_native.dll);
	}
	g_native_crash.active.store(false, std::memory_order_relaxed);
#endif
	g_native = {};
}

bool DOSBoxNativeRenderer_Available(void)
{
	return g_native.enabled && g_native.api;
}

const char *DOSBoxNativeRenderer_GetSourceName(void)
{
	return DOSBoxNativeRenderer_Available() ? g_native.source_name.c_str() : "";
}

size_t DOSBoxNativeRenderer_GetHookCount(void)
{
	if (!DOSBoxNativeRenderer_Available() || !g_native.descriptor)
		return 0;
	return g_native.api->abi_version >= MW2ER_ABI_VERSION
	             ? g_native.descriptor->binding_count
	             : 1;
}

bool DOSBoxNativeRenderer_GetHook(size_t index,
                                  std::string *exe_name_upper,
                                  uint32_t *event,
                                  uint32_t *kind,
                                  uint32_t *reloc_eip)
{
	if (!DOSBoxNativeRenderer_Available() || !g_native.descriptor ||
	    !exe_name_upper || !event || !kind || !reloc_eip ||
	    index >= DOSBoxNativeRenderer_GetHookCount())
		return false;
	*exe_name_upper = uppercase_ascii_copy(g_native.descriptor->executable_name);
	if (g_native.api->abi_version >= MW2ER_ABI_VERSION) {
		*event = g_native.descriptor->bindings[index].event;
		*kind = g_native.descriptor->bindings[index].kind;
		*reloc_eip = g_native.descriptor->bindings[index].reloc_eip;
	} else {
		*event = MW2ER_EVENT_LEGACY_RENDER;
		*kind = MW2ER_HOOK_RENDER;
		*reloc_eip = g_native.render_hook_reloc;
	}
	return true;
}

void DOSBoxNativeRenderer_NotifyOpenGLContextCreated(const uint64_t generation)
{
	g_native.context_generation = generation;
	g_native.context_ready = false;
	memset(&g_native.viewport, 0, sizeof(g_native.viewport));
}

void DOSBoxNativeRenderer_NotifyOpenGLContextLost(void)
{
	NativeCallScope crash_scope("on_gl_context_lost");
#if C_OPENGL && defined(C_SDL2)
    if (g_native.owned_context &&
        SDL_GL_MakeCurrent(g_native.context_window, g_native.owned_context) != 0)
        E_Exit("Unable to restore native renderer GL context for safe teardown: %s", SDL_GetError());
    if (g_native.owned_context && g_native.api)
        g_native.api->on_gl_context_lost();
    g_native.owned_context = nullptr;
    g_native.context_window = nullptr;
#endif
	g_native.context_ready = false;
	g_native.context_generation = 0;
	memset(&g_native.viewport, 0, sizeof(g_native.viewport));
}

bool DOSBoxNativeRenderer_InvokeOpenGLInit(const ModOpenGLState& state)
{
	if (!DOSBoxNativeRenderer_Available())
		return false;
#if C_OPENGL && defined(C_SDL2)
	if (SDL_GL_GetCurrentContext() == NULL) {
		LOG_MSG("NATIVE RENDERER ERROR: OpenGL init requested without a current context");
		return false;
	}
#endif
	g_native.viewport = make_viewport(state);
#if C_OPENGL && defined(C_SDL2)
    if (g_native.owned_context && g_native.owned_context != SDL_GL_GetCurrentContext()) {
        disable_renderer("unreleased GL context", MW2ER_ERR_GL);
        return false;
    }
	if (g_native.context_ready &&
	    g_native.context_generation == state.context_generation)
		return true;
    g_native.owned_context = SDL_GL_GetCurrentContext();
    g_native.context_window = SDL_GL_GetCurrentWindow();
#endif
    g_native.context_generation = state.context_generation;
    NativeCallScope crash_scope("on_gl_context");
	if (!call_ok("on_gl_context", g_native.api->on_gl_context(&g_native.viewport)))
		return false;
	g_native.context_ready = true;
	return true;
}

static std::string host_opened_path(const char *dos_name)
{
	if (!dos_name || !dos_name[0])
		return {};
	char fullname[DOS_PATHLENGTH] = {};
	uint8_t drive = 0;
	const uint16_t saved_error = dos.errorcode;
	const bool named = DOS_MakeName(dos_name, fullname, &drive);
	dos.errorcode = saved_error;
	if (!named || drive >= DOS_DRIVES || !Drives[drive])
		return {};
	localDrive *local = dynamic_cast<localDrive *>(Drives[drive]);
	if (!local)
		return {};
	const std::string opened_name = fullname;
	Overlay_Drive *overlay = dynamic_cast<Overlay_Drive *>(local);
	if (strlen(local->getBasedir()) + opened_name.size() >= CROSS_LEN ||
	    (overlay && strlen(overlay->getOverlaydir()) + opened_name.size() >= CROSS_LEN))
		return {};
	const std::string host_name = overlay
	                                   ? overlay->GetHostName(opened_name.c_str())
	                                   : local->GetHostName(opened_name.c_str());
	char utf8[CROSS_LEN] = {};
	if (host_name.empty() || !CodePageGuestToHostUTF8(utf8, host_name.c_str()))
		return {};
#if defined(WIN32) && !defined(HX_DOS)
	wchar_t wide[CROSS_LEN] = {};
	wchar_t absolute[CROSS_LEN] = {};
	if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1,
	                         wide, CROSS_LEN))
		return {};
	const DWORD length = GetFullPathNameW(wide, CROSS_LEN, absolute, NULL);
	if (length == 0 || length >= CROSS_LEN ||
	    !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, absolute, -1,
	                         utf8, CROSS_LEN, NULL, NULL))
		return {};
#endif
	return utf8;
}

static const char *dos_basename(const char *name)
{
	const char *basename = name;
	for (const char *cursor = name; *cursor; ++cursor) {
		if (*cursor == ':' || *cursor == '/' || *cursor == '\\')
			basename = cursor + 1;
	}
	return basename;
}

static bool same_ascii_name(const char *left, const char *right)
{
	while (*left && *right) {
		if (std::toupper(static_cast<unsigned char>(*left++)) !=
		    std::toupper(static_cast<unsigned char>(*right++)))
			return false;
	}
	return *left == *right;
}

void DOSBoxNativeRenderer_OnFileOpened(const char *dos_name)
{
	if (!DOSBoxNativeRenderer_Available() ||
	    g_native.api->abi_version < MW2ER_ABI_VERSION ||
	    !g_native.descriptor->observed_file_name || !dos_name || !dos_name[0])
		return;
	if (same_ascii_name(dos_basename(dos_name),
	                    g_native.descriptor->observed_file_name))
		g_native.opened_path = host_opened_path(dos_name);
}

static int32_t resolve_opened_path(void *, const char *name,
                                   char *host_path, uint32_t capacity)
{
	if (!name || !g_native.descriptor->observed_file_name ||
	    !same_ascii_name(name, g_native.descriptor->observed_file_name) ||
	    !host_path || capacity == 0)
		return MW2ER_ERR_INVALID_ARGUMENT;
	host_path[0] = '\0';
	if (g_native.opened_path.empty())
		return MW2ER_ERR_NOT_READY;
	if (g_native.opened_path.size() >= capacity)
		return MW2ER_ERR_INVALID_ARGUMENT;
	memcpy(host_path, g_native.opened_path.c_str(), g_native.opened_path.size() + 1);
	return MW2ER_OK;
}

bool DOSBoxNativeRenderer_MissionBegin(const int64_t delta)
{
	if (!DOSBoxNativeRenderer_Available())
		return false;
	Mw2erMemoryView memory = {};
	if (!make_memory_view(delta, &memory))
		return false;
	const uint64_t probe = static_cast<uint64_t>(memory.delta) +
	                       g_native.render_hook_reloc;
	if (probe >= memory.size ||
	    memory.bytes[probe] != mem_readb(static_cast<PhysPt>(probe))) {
		LOG_MSG("NATIVE RENDERER ERROR: zero-copy guest view identity probe failed");
		return false;
	}
	LOG_MSG("NATIVE RENDERER: zero-copy guest RAM accepted (%lu bytes, delta 0x%08lX, paging off)",
	        static_cast<unsigned long>(memory.size),
	        static_cast<unsigned long>(memory.delta));
	NativeCallScope crash_scope("mission_begin");
	if (g_native.api->abi_version >= MW2ER_ABI_VERSION) {
		Mw2erSessionInfo session = {};
		session.struct_size = sizeof(session);
		session.session_generation = ++g_native.next_session_generation;
		session.profile_id = g_native.descriptor->name;
		session.executable_name = g_native.descriptor->executable_name;
		session.resolve_opened_path = resolve_opened_path;
		crash_scope.set_operation("begin_session");
		if (!call_ok("begin_session", g_native.api->begin_session(&session)))
			return false;
		g_native.session_generation = session.session_generation;

		Mw2erMissionInfo mission = {};
		mission.struct_size = sizeof(mission);
		mission.session_generation = g_native.session_generation;
		mission.mission_generation = ++g_native.next_mission_generation;
		mission.resource_generation = mission.mission_generation;
		crash_scope.set_operation("begin_mission");
		if (!call_ok("begin_mission", g_native.api->begin_mission(&mission))) {
			crash_scope.set_operation("end_session");
			g_native.api->end_session(g_native.session_generation);
			g_native.session_generation = 0;
			return false;
		}
		g_native.mission_generation = mission.mission_generation;
		g_native.resource_generation = mission.resource_generation;
	} else {
		crash_scope.set_operation("mission_begin");
		if (!call_ok("mission_begin", g_native.api->mission_begin(&memory)))
			return false;
	}
	g_native.mission = true;
	g_native.delta = delta;
	return true;
}

void DOSBoxNativeRenderer_MissionEnd(void)
{
	release_renderer_policy();
	NativeCallScope crash_scope("mission_end");
	if (g_native.mission && g_native.api) {
		if (g_native.api->abi_version >= MW2ER_ABI_VERSION) {
			crash_scope.set_operation("end_mission");
			g_native.api->end_mission(g_native.mission_generation);
			crash_scope.set_operation("end_session");
			g_native.api->end_session(g_native.session_generation);
		} else if (g_native.api->mission_end) {
			g_native.api->mission_end();
		}
	}
	g_native.mission = false;
	g_native.session_generation = 0;
	g_native.mission_generation = 0;
	g_native.resource_generation = 0;
	g_native.delta = 0;
}

bool DOSBoxNativeRenderer_InvokeHook(const uint32_t event,
                                     const uint32_t kind,
                                     const ModFrameState& frame,
                                     const int64_t delta)
{
	if (!DOSBoxNativeRenderer_Available() ||
	    (kind == MW2ER_HOOK_RENDER && !g_native.context_ready))
		return false;
	// Process activation can precede the point where DOS paging has settled.
	// Retry here so a deferred zero-copy binding does not permanently prevent
	// the renderer from starting for the lifetime of the mission.
	if (!g_native.mission && !DOSBoxNativeRenderer_MissionBegin(delta))
		return false;
#if C_OPENGL && defined(C_SDL2)
	if (kind == MW2ER_HOOK_RENDER && SDL_GL_GetCurrentContext() == NULL) {
		LOG_MSG("NATIVE RENDERER ERROR: render hook has no current OpenGL context");
		return false;
	}
#endif
	Mw2erMemoryView memory = {};
	if (!make_memory_view(delta, &memory))
		return false;
	NativeCallScope crash_scope("capture", frame.frame);
	Mw2erFrameInfo frame_info = {};
	frame_info.struct_size = sizeof(frame_info);
	frame_info.frame = frame.frame;
	frame_info.time_seconds = frame.time_seconds;
#if C_OPENGL && defined(C_SDL2)
	if (kind != MW2ER_HOOK_RENDER)
		frame_info.time_seconds = static_cast<double>(SDL_GetPerformanceCounter()) *
		                          g_native.counter_to_seconds;
#endif
	frame_info.frame_delta_seconds = frame.frame_delta_seconds;
	if (g_native.api->abi_version >= MW2ER_ABI_VERSION) {
		Mw2erCaptureInput capture = {};
		capture.struct_size = sizeof(capture);
		capture.event = event;
		capture.session_generation = g_native.session_generation;
		capture.mission_generation = g_native.mission_generation;
		capture.resource_generation = g_native.resource_generation;
		capture.memory = memory;
		capture.viewport = g_native.viewport;
		capture.frame = frame_info;
		if (!frame_call_ok("capture", g_native.api->capture(&capture)))
			return false;
		if (has_resource_service()) {
			crash_scope.set_operation("resources_pending");
			if (g_native.api->resources_pending(g_native.resource_generation) != 0)
				MOD_RequestSafePoint();
		}
		if (kind != MW2ER_HOOK_RENDER)
			return true;
		crash_scope.set_operation("seal_frame");
		if (!frame_call_ok("seal_frame", g_native.api->seal_frame(frame.frame)))
			return false;

		Mw2erRenderRequest request = {};
		request.struct_size = sizeof(request);
		request.required_layers = MW2ER_LAYER_SCENE | MW2ER_LAYER_OVERLAY;
		request.session_generation = g_native.session_generation;
		request.mission_generation = g_native.mission_generation;
		request.resource_generation = g_native.resource_generation;
		request.frame = frame.frame;
		request.viewport = g_native.viewport;
		crash_scope.set_operation("render_frame");
		if (!frame_call_ok("render_frame", g_native.api->render_frame(&request)))
			return false;

		Mw2erPublishResult publication = {};
		publication.struct_size = sizeof(publication);
		crash_scope.set_operation("publish_frame");
		if (!frame_call_ok("publish_frame", g_native.api->publish_frame(&publication)))
			return false;
		if (publication.session_generation != g_native.session_generation ||
		    publication.mission_generation != g_native.mission_generation ||
		    publication.resource_generation != g_native.resource_generation ||
		    publication.source_frame != frame.frame ||
		    publication.context_generation != g_native.context_generation ||
		    (publication.completed_layers &
		     (MW2ER_LAYER_SCENE | MW2ER_LAYER_OVERLAY)) !=
		            (MW2ER_LAYER_SCENE | MW2ER_LAYER_OVERLAY)) {
			disable_renderer("publish_frame metadata", MW2ER_ERR_INVALID_ARGUMENT);
			return false;
		}
		return (publication.coverage & MW2ER_COVERAGE_SCENE) != 0;
	}
	crash_scope.set_operation("bind_frame");
	if (!call_ok("bind_frame",
	             g_native.api->bind_frame(&memory, &g_native.viewport, &frame_info)))
		return false;
	crash_scope.set_operation("render_scene");
	if (!call_ok("render_scene", g_native.api->render_scene()))
		return false;
	crash_scope.set_operation("render_hud");
	if (!call_ok("render_hud", g_native.api->render_hud()))
		return false;
	crash_scope.set_operation("publish");
	if (!call_ok("publish", g_native.api->publish()))
		return false;
	return true;
}

void DOSBoxNativeRenderer_ServiceResources(void)
{
	if (!DOSBoxNativeRenderer_Available() || !g_native.mission ||
	    !has_resource_service())
		return;
	NativeCallScope crash_scope("resources_pending");
	if (g_native.api->resources_pending(g_native.resource_generation) == 0)
		return;
	Mw2erMemoryView memory = {};
	if (!make_memory_view(g_native.delta, &memory))
		return;
	Mw2erResourceService service = {};
	service.struct_size = sizeof(service);
	service.max_resources = 16;
	HostResourceContext context = {};
	if (!MOD_ReadMemoryU32(0x000AEA8Cu, &context.acquire_context) ||
	    !MOD_ReadMemoryU32(resource_type_reloc(MW2ER_RESOURCE_CEL),
	                       &context.cel_type) ||
	    !MOD_ReadMemoryU32(resource_type_reloc(MW2ER_RESOURCE_POLY),
	                       &context.poly_type))
		return;
	service.user = &context;
	service.memory = memory;
	service.acquire = acquire_resource;
	service.release = release_resource;
	Mw2erResourceProgress progress = {};
	progress.struct_size = sizeof(progress);
	crash_scope.set_operation("service_resources");
	const int32_t result = g_native.api->service_resources(&service, &progress);
	if (context.interrupted) std::rethrow_exception(context.interrupted);
	if (result == MW2ER_ERR_NOT_READY)
		return; // Retry from a later guest hook after the context can advance.
	if (result != MW2ER_OK) {
		disable_renderer("service_resources", result);
		return;
	}
	if (progress.pending != 0)
		MOD_RequestSafePoint();
}

bool DOSBoxNativeRenderer_InvokeCompositor(const ModOpenGLState& state)
{
    unsigned audit_flags = 0;
    if (MIXER_TimingAuditEnabled) {
        audit_flags = (DOSBoxNativeRenderer_Available() ? 1u : 0u) |
                      (g_native.mission ? 2u : 0u) | (g_native.context_ready ? 4u : 0u);
    }
	if (!DOSBoxNativeRenderer_Available() || !g_native.mission || !g_native.context_ready) {
        if (MIXER_TimingAuditEnabled) MIXER_TimingAuditNativeState(MW2ER_ERR_NOT_READY, audit_flags);
		return false;
    }
#if C_OPENGL && defined(C_SDL2)
	if (SDL_GL_GetCurrentContext() == NULL) {
        if (MIXER_TimingAuditEnabled) MIXER_TimingAuditNativeState(MW2ER_ERR_NOT_READY, audit_flags);
		LOG_MSG("NATIVE RENDERER ERROR: compositor has no current OpenGL context");
		return false;
	}
    if (MIXER_TimingAuditEnabled) audit_flags |= 8u;
#endif
	g_native.viewport = make_viewport(state);
	NativeCallScope crash_scope("composite_frame", state.present_count);
	int32_t result = MW2ER_ERR_NOT_READY;
	if (g_native.api->abi_version >= MW2ER_ABI_VERSION) {
		Mw2erPresentInfo present = {};
		present.struct_size = sizeof(present);
		present.present_count = state.present_count;
#if C_OPENGL && defined(C_SDL2)
		present.time_seconds = static_cast<double>(SDL_GetPerformanceCounter()) *
		                       g_native.counter_to_seconds;
#endif
		present.viewport = g_native.viewport;
		Mw2erPresentResult presented = {};
		presented.struct_size = sizeof(presented);
		result = g_native.api->composite_frame(&present, &presented);
		if (result == MW2ER_OK) {
			MOD_SetFramePacingSuspended(presented.suspend_frame_pacing != 0);
			MOD_SetFramePacingContinuousPresentation(presented.continuous != 0);
		}
        if (MIXER_TimingAuditEnabled) {
            audit_flags |= (presented.presented ? 16u : 0u) |
                           (presented.suspend_frame_pacing ? 32u : 0u) |
                           (presented.continuous ? 64u : 0u);
            MIXER_TimingAuditNativeState(result, audit_flags);
        }
		if (result == MW2ER_OK && !presented.presented)
			return false;
	} else {
		crash_scope.set_operation("composite");
		result = g_native.api->composite(&g_native.viewport);
        if (MIXER_TimingAuditEnabled) MIXER_TimingAuditNativeState(result, audit_flags);
	}
	if (result == MW2ER_ERR_NOT_READY)
		return false;
	return call_ok("composite", result);
}

void DOSBoxNativeRenderer_GetLastTiming(double *extract_ms,
                                       double *draw_submit_ms)
{
	if (extract_ms)
		*extract_ms = 0.0;
	if (draw_submit_ms)
		*draw_submit_ms = 0.0;
	if (g_native.api && g_native.api->last_cpu_timing) {
		NativeCallScope crash_scope("last_cpu_timing");
		g_native.api->last_cpu_timing(extract_ms, draw_submit_ms);
	}
}
