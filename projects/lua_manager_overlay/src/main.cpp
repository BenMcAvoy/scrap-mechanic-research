#include <windows.h>
#include <tlhelp32.h>
#include <dbghelp.h>
#include <d3d11.h>
#include <dxgi.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <array>
#include <string>
#include <string_view>
#include <vector>

#include "imgui.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"
#include "memorylib/memorylib.hpp"

extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace {

// The LuaManager detours below are backed by current-binary IDA evidence, but
// remain opt-in while the current injector/runtime is validated. The overlay
// itself is the stable default configuration.
constexpr bool kEnableInitializeHook = false;
constexpr bool kEnableScriptLoadHook = false;
// IDA shows this dispatcher consumes the manager in RCX and the frame delta in
// XMM1.
constexpr bool kEnableClientUpdateHook = false;
constexpr bool kEnableFixedUpdateHook = false;
constexpr bool kEnableVmRefreshHook = false;
constexpr bool kEnableClientDataHook = false;
constexpr bool kEnableLifecycleHook = false;
constexpr bool kProbeClientHookWithoutOriginal = false;
// Keep diagnostics inside the DLL until the game-console wrapper ABI is
// independently proven. The overlay and file log remain fully available.
constexpr bool kEnableGameConsoleOutput = false;
constexpr bool kCaptureInternalState = true;
// Current-IDB qword_141AA25A8 minus imagebase 0x140000000. This is
// version-specific and is validated against the LuaManager constructor.
constexpr std::uintptr_t kLuaManagerSingletonRva = 0x1AA25A8;

using PresentFn = HRESULT(__stdcall *)(IDXGISwapChain *, UINT, UINT);
using ResizeBuffersFn = HRESULT(__stdcall *)(IDXGISwapChain *, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using InitializeFn = void *(__fastcall *)(void *, void *);
using ScriptLoadFn = void *(__fastcall *)(void *, void *);
// IDA confirms this dispatcher receives the LuaManager pointer in RCX and the
// frame delta in XMM1 before passing that delta to Lua as a number.
using ClientUpdateFn = std::intptr_t(__fastcall *)(void *, float);
using FixedUpdateFn = char *(__fastcall *)(void *);
using VmRefreshFn = std::intptr_t(__fastcall *)(void *, std::intptr_t);
using ClientDataUpdateFn = std::intptr_t(__fastcall *)(std::intptr_t, std::intptr_t, std::intptr_t, std::intptr_t,
    std::intptr_t *, int, unsigned int *, int, int, void *);
// Current-IDB lifecycle dispatcher: RCX manager, RDX lifecycle argument,
// R8 byte lifecycle kind, R9 flags.
using LifecycleFn = char(__fastcall *)(void *, std::intptr_t, std::uint8_t, int);
// Resolved UTILS::Console wrapper ABI from the matched Scrap Mechanic IDB;
// the resolver is string/xref based so it is not tied to a stale build label.
//   (category, flags, source_path, source_line, &source_name, unused,
//    &message_line, unused, message)
// The wrapper dereferences arguments 5, 7, and 9 before forwarding to the
// UTILS::Console vtable. Keep the unused slots explicit so the Win64 stack
// arguments remain correctly positioned.
using GameConsoleWriteFn = std::int64_t(__fastcall *)(int, unsigned int, const char *, unsigned int,
    const char **, std::uintptr_t, const unsigned int *, std::uintptr_t, const char *);

struct ResolvedFunction {
    const char *label{};
    const char *anchor{};
    void *address{};
    bool resolved{};
    std::string error;
    std::uint64_t calls{};
    std::uintptr_t last_this{};
    DWORD last_thread{};
    std::uint64_t last_tick{};
};

struct CallbackEntryState {
    bool readable{};
    std::uintptr_t address{};
    std::uint64_t field_0x10{};
    std::uint64_t field_0x18{};
    std::uint32_t field_0x20{};
    std::uint32_t field_0x24{};
    std::uint32_t field_0x28{};
    std::uint8_t field_0x30{};
    std::uint8_t field_0x38{};
};

struct CallbackVectorState {
    std::size_t count{};
    std::array<CallbackEntryState, 8> entries{};
};

struct HashNodeState {
    bool readable{};
    std::uintptr_t address{};
    std::uint64_t key{};
    std::array<std::uint64_t, 3> payload{};
};

struct HashContainerState {
    bool readable{};
    std::size_t object_offset{};
    float max_load_factor{};
    std::uintptr_t head{};
    std::uint64_t size{};
    std::uintptr_t buckets{};
    std::uint64_t bucket_mask{};
    std::uint64_t bucket_count{};
    std::array<HashNodeState, 16> nodes{};
};

// Version-specific layout recovered from the current IDB's LuaManager
// dispatchers (ScrapMechanic.current.exe.i64). These are deliberately raw
// fields: no game STL/Lua types cross the DLL boundary.
struct LuaManagerInternalState {
    static constexpr std::size_t raw_qword_count = 0x368 / sizeof(std::uint64_t);
    bool readable{};
    std::uintptr_t self{};
    std::int32_t callback_depth{};          // +0x48, dispatcher reentrancy guard
    std::int64_t callback_index{};          // +0x4C, active callback/index sentinel
    std::uint8_t callback_active{};        // +0x58
    std::uint8_t callback_guard{};          // +0x59
    std::uint8_t callback_kind{};           // +0x5A
    std::uint8_t is_server{};               // +0x354, set by LuaManager constructor
    std::uintptr_t callback_context_0x20{}; // transient callback context
    std::uintptr_t callback_context_0x28{};
    std::uintptr_t callback_context_0x30{};
    std::uintptr_t callback_context_0x38{};
    std::uintptr_t callback_context_0x40{};
    std::uint32_t callback_context_id{};    // +0x4C
    std::uint32_t callback_context_type{};  // +0x50
    std::uint32_t callback_context_flags{}; // +0x54
    std::uintptr_t container_0x0F8_begin{};
    std::uintptr_t container_0x100_end{};
    std::uintptr_t container_0x110_begin{};
    std::uintptr_t container_0x118_end{};
    std::uintptr_t container_0x150_begin{};
    std::uintptr_t container_0x158_end{};
    std::uintptr_t container_0x190_begin{};
    std::uintptr_t container_0x198_end{};
    std::uintptr_t container_0x1D0_begin{};
    std::uintptr_t container_0x1D8_end{};
    std::uintptr_t registry_storage{};      // +0x218, raw storage pointer
    std::uintptr_t registry_buckets{};      // +0x228, raw bucket storage
    std::uintptr_t registry_mask{};         // +0x240, hash mask/table metadata
    std::uintptr_t container_0x250{};
    std::uintptr_t container_0x258{};
    std::uintptr_t container_0x268{};
    std::uintptr_t container_mask_0x280{};
    std::uintptr_t container_0x308{};
    std::uintptr_t container_0x310{};
    std::uintptr_t container_0x320{};
    std::uintptr_t container_0x338{};
    std::uint32_t callback_count{};         // +0x348, createScriptInstance counter
    std::uint32_t fixed_cursor{};           // +0x350, fixed-update work cursor
    std::uintptr_t server_callbacks_begin{}; // +0x290
    std::uintptr_t server_callbacks_end{};   // +0x298
    std::uintptr_t server_callbacks_capacity{}; // +0x2A0
    std::uintptr_t fixed_callbacks_begin{};  // +0x2A8
    std::uintptr_t fixed_callbacks_end{};    // +0x2B0
    std::uintptr_t fixed_callbacks_capacity{}; // +0x2B8
    std::uintptr_t client_fixed_begin{};     // +0x2C0
    std::uintptr_t client_fixed_end{};       // +0x2C8
    std::uintptr_t client_fixed_capacity{};  // +0x2D0
    std::uintptr_t client_update_begin{};    // +0x2D8
    std::uintptr_t client_update_end{};      // +0x2E0
    std::uintptr_t client_update_capacity{}; // +0x2E8
    std::uintptr_t receive_update_begin{};   // +0x2F0
    std::uintptr_t receive_update_end{};     // +0x2F8
    std::uintptr_t receive_update_capacity{}; // +0x300
    std::uintptr_t lua_vm_shared_ptr{};      // +0x358, std::shared_ptr<LuaVM>::get()
    std::uintptr_t lua_vm_control_block{};   // +0x360, shared_ptr control block
    CallbackVectorState server_callback_entries{};
    CallbackVectorState fixed_callback_entries{};
    CallbackVectorState client_fixed_entries{};
    CallbackVectorState client_update_entries{};
    CallbackVectorState receive_update_entries{};
    HashContainerState registered_callback_hashes{}; // +0x110, unordered_set<uint64_t>
    HashContainerState callback_type_registry{};     // +0x210, unordered_map<uint8_t, ...>
    std::array<std::uint64_t, raw_qword_count> raw_qwords{};
    std::array<std::uint8_t, raw_qword_count> raw_valid{};
};

struct OverlayState {
    std::mutex mutex;
    HMODULE game{};
    HWND window{};
    WNDPROC original_wndproc{};
    ID3D11Device *device{};
    ID3D11DeviceContext *context{};
    ID3D11RenderTargetView *render_target{};
    bool imgui_ready{};
    bool visible{true};
    bool menu_key_down{};
    std::string last_mode;
    std::string last_class;
    std::string last_error;
    bool game_console_resolved{};
    std::uint64_t game_console_writes{};
    bool mode_scalar_resolved{};
    std::int32_t raw_mode_value{};
    std::uintptr_t mode_scalar_address{};
    std::intptr_t last_lifecycle_argument{};
    std::uint8_t last_lifecycle_kind{};
    int last_lifecycle_flags{};
    LuaManagerInternalState internals{};
    std::atomic_bool stopping{};
    ResolvedFunction initialize{"LuaManager initialize", "Initializing LuaManager as client"};
    ResolvedFunction script_load{"GameScript select/load", "Z:\\Build\\sm_steam\\ContraptionCommon\\GameScript.cpp"};
    ResolvedFunction client_update{"client_onUpdate dispatcher", "client_onUpdate callback - callback during ongoing callback '"};
    ResolvedFunction fixed_update{"fixed-update dispatcher", "server_onFixedUpdate callback - callback during ongoing callback '"};
    ResolvedFunction vm_refresh{"VM refresh", "onRefreshVMCallback - create class during ongoing callback  '"};
    ResolvedFunction client_data{"client data update", "client_onClientDataUpdate - callback during ongoing callback '"};
    ResolvedFunction lifecycle{"lifecycle dispatcher", "!bInstant || ((tls_threadContext == ThreadContext::Synchronized) || (tls_threadContext == ThreadContext::PreRend) || (tls_threadContext == ThreadContext::Logic || tls_threadContext == ThreadContext::LogicSync))"};
};

OverlayState g_state;
HMODULE g_self_module{};
std::atomic_bool g_unload_requested{};
std::atomic_bool g_bootstrap_finished{};
std::atomic_bool g_cleanup_done{};
std::atomic_bool g_overlay_frame_marked{};
std::atomic_uint32_t g_present_in_flight{};
std::atomic_uint32_t g_client_detour_phase{};
PVOID g_crash_handler{};
volatile LONG g_crash_report_in_progress{};
mem::hook::Function<PresentFn> g_present_hook;
mem::hook::Function<ResizeBuffersFn> g_resize_hook;
mem::hook::Function<InitializeFn> g_initialize_hook;
mem::hook::Function<ScriptLoadFn> g_script_load_hook;
mem::hook::Function<ClientUpdateFn> g_client_update_hook;
mem::hook::Function<FixedUpdateFn> g_fixed_update_hook;
mem::hook::Function<VmRefreshFn> g_vm_refresh_hook;
mem::hook::Function<ClientDataUpdateFn> g_client_data_hook;
mem::hook::Function<LifecycleFn> g_lifecycle_hook;
std::atomic<GameConsoleWriteFn> g_game_console_write{};
std::atomic<const std::int32_t *> g_game_script_mode{};
std::mutex g_file_log_mutex;

LONG CALLBACK crash_report_vectored_handler(PEXCEPTION_POINTERS exception_info);

class OtherThreadsSuspended {
  public:
    OtherThreadsSuspended() {
        const auto process_id = GetCurrentProcessId();
        const auto current_thread = GetCurrentThreadId();
        const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE)
            return;
        THREADENTRY32 entry{sizeof(entry)};
        if (Thread32First(snapshot, &entry)) {
            do {
                if (entry.th32OwnerProcessID != process_id || entry.th32ThreadID == current_thread)
                    continue;
                const auto thread = OpenThread(THREAD_SUSPEND_RESUME, FALSE, entry.th32ThreadID);
                if (thread && SuspendThread(thread) != static_cast<DWORD>(-1))
                    threads_.push_back(thread);
                else if (thread)
                    CloseHandle(thread);
            } while (Thread32Next(snapshot, &entry));
        }
        CloseHandle(snapshot);
    }

    ~OtherThreadsSuspended() {
        for (const auto thread : threads_) {
            ResumeThread(thread);
            CloseHandle(thread);
        }
    }

    OtherThreadsSuspended(const OtherThreadsSuspended &) = delete;
    OtherThreadsSuspended &operator=(const OtherThreadsSuspended &) = delete;

  private:
    std::vector<HANDLE> threads_;
};

void loader_marker(const char *message) {
    char temp_path[MAX_PATH]{};
    const auto length = GetTempPathA(static_cast<DWORD>(sizeof(temp_path)), temp_path);
    if (length == 0 || length >= sizeof(temp_path))
        return;
    const auto path = std::string(temp_path) + "lua_manager_overlay.log";
    const auto handle = CreateFileA(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return;
    DWORD written{};
    const auto size = static_cast<DWORD>(std::strlen(message));
    WriteFile(handle, message, size, &written, nullptr);
    WriteFile(handle, "\r\n", 2, &written, nullptr);
    CloseHandle(handle);
}

void write_ready_marker() {
    char temp_path[MAX_PATH]{};
    const auto length = GetTempPathA(static_cast<DWORD>(sizeof(temp_path)), temp_path);
    if (length == 0 || length >= sizeof(temp_path))
        return;
    const auto path = std::string(temp_path) + "lua_manager_overlay.ready";
    const auto handle = CreateFileA(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return;
    static constexpr char marker[] = "LuaManager overlay bootstrap complete\r\n";
    DWORD written{};
    WriteFile(handle, marker, static_cast<DWORD>(sizeof(marker) - 1), &written, nullptr);
    CloseHandle(handle);
}

void write_overlay_frame_marker() {
    char temp_path[MAX_PATH]{};
    const auto length = GetTempPathA(static_cast<DWORD>(sizeof(temp_path)), temp_path);
    if (length == 0 || length >= sizeof(temp_path))
        return;
    const auto path = std::string(temp_path) + "lua_manager_overlay.frame";
    const auto handle = CreateFileA(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return;
    static constexpr char marker[] = "LuaManager overlay frame rendered\r\n";
    DWORD written{};
    WriteFile(handle, marker, static_cast<DWORD>(sizeof(marker) - 1), &written, nullptr);
    CloseHandle(handle);
}

LONG CALLBACK crash_report_vectored_handler(PEXCEPTION_POINTERS exception_info) {
    if (!exception_info || !exception_info->ExceptionRecord)
        return EXCEPTION_CONTINUE_SEARCH;

    const auto code = exception_info->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_INT_DIVIDE_BY_ZERO &&
        code != EXCEPTION_STACK_OVERFLOW)
        return EXCEPTION_CONTINUE_SEARCH;

    // This handler is deliberately registered last and never consumes an
    // exception. The game's VEH gets first opportunity to handle it. We only
    // produce evidence if the exception continues through the VEH chain.
    if (InterlockedExchange(&g_crash_report_in_progress, 1) != 0)
        return EXCEPTION_CONTINUE_SEARCH;

    char temp_path[MAX_PATH]{};
    const auto length = GetTempPathA(static_cast<DWORD>(sizeof(temp_path)), temp_path);
    if (length > 0 && length < sizeof(temp_path)) {
        char stem[MAX_PATH]{};
        _snprintf_s(stem, sizeof(stem), _TRUNCATE, "lua_manager_overlay-crash-%lu-%llu",
            GetCurrentProcessId(), static_cast<unsigned long long>(GetTickCount64()));
        const std::string base = std::string(temp_path) + stem;

        const auto report = CreateFileA((base + ".txt").c_str(), GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (report != INVALID_HANDLE_VALUE) {
            auto writef = [report](const char *format, ...) {
                char line[1024]{};
                va_list arguments;
                va_start(arguments, format);
                const auto count = _vsnprintf_s(line, sizeof(line), _TRUNCATE, format, arguments);
                va_end(arguments);
                if (count <= 0)
                    return;
                DWORD written{};
                WriteFile(report, line, static_cast<DWORD>(count), &written, nullptr);
            };

            const auto record = exception_info->ExceptionRecord;
            writef("exception=0x%08lX address=%p flags=0x%08lX thread=%lu\r\n",
                static_cast<unsigned long>(code), record->ExceptionAddress,
                static_cast<unsigned long>(record->ExceptionFlags),
                static_cast<unsigned long>(GetCurrentThreadId()));
            writef("parameter_count=%lu parameter0=0x%llX parameter1=0x%llX client_detour_phase=%lu\r\n",
                static_cast<unsigned long>(record->NumberParameters),
                record->NumberParameters > 0 ? record->ExceptionInformation[0] : 0ull,
                record->NumberParameters > 1 ? record->ExceptionInformation[1] : 0ull,
                static_cast<unsigned long>(g_client_detour_phase.load(std::memory_order_relaxed)));

            CONTEXT context{};
            if (exception_info->ContextRecord) {
                context = *exception_info->ContextRecord;
                writef("rip=0x%llX rsp=0x%llX rbp=0x%llX rax=0x%llX rbx=0x%llX rcx=0x%llX rdx=0x%llX\r\n",
                    context.Rip, context.Rsp, context.Rbp, context.Rax, context.Rbx, context.Rcx, context.Rdx);
                writef("rsi=0x%llX rdi=0x%llX r8=0x%llX r9=0x%llX r10=0x%llX r11=0x%llX r12=0x%llX r13=0x%llX r14=0x%llX r15=0x%llX\r\n",
                    context.Rsi, context.Rdi, context.R8, context.R9, context.R10, context.R11,
                    context.R12, context.R13, context.R14, context.R15);

                // Walk from the saved exception context, not from this VEH
                // function. This preserves the crashing thread's call chain.
                HANDLE process = GetCurrentProcess();
                HANDLE thread = GetCurrentThread();
                SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
                SymInitialize(process, nullptr, TRUE);
                STACKFRAME64 frame{};
                frame.AddrPC.Offset = context.Rip;
                frame.AddrPC.Mode = AddrModeFlat;
                frame.AddrFrame.Offset = context.Rbp;
                frame.AddrFrame.Mode = AddrModeFlat;
                frame.AddrStack.Offset = context.Rsp;
                frame.AddrStack.Mode = AddrModeFlat;
                writef("stack:\r\n");
                for (unsigned index = 0; index < 64; ++index) {
                    const auto walked = StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame,
                        &context, nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr);
                    if (!walked || frame.AddrPC.Offset == 0)
                        break;
                    const auto module_base = SymGetModuleBase64(process, frame.AddrPC.Offset);
                    writef("  #%02u pc=0x%llX module_base=0x%llX sp=0x%llX\r\n", index,
                        frame.AddrPC.Offset, module_base, frame.AddrStack.Offset);
                }
            } else {
                writef("no_context_record=1\r\n");
            }
            CloseHandle(report);
        }

        const auto dump = CreateFileA((base + ".dmp").c_str(), GENERIC_WRITE,
            FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (dump != INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION dump_exception{};
            dump_exception.ThreadId = GetCurrentThreadId();
            dump_exception.ExceptionPointers = exception_info;
            dump_exception.ClientPointers = FALSE;
            MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), dump,
                static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules |
                    MiniDumpWithIndirectlyReferencedMemory), &dump_exception, nullptr, nullptr);
            CloseHandle(dump);
        }
        loader_marker("fatal exception passed through game VEH; crash report written to %TEMP%");
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void log_line(std::string_view message) {
    const std::string owned(message);
    {
        char temp_path[MAX_PATH]{};
        const auto length = GetTempPathA(static_cast<DWORD>(sizeof(temp_path)), temp_path);
        if (length > 0 && length < sizeof(temp_path)) {
            std::lock_guard file_lock(g_file_log_mutex);
            std::ofstream file(std::string(temp_path) + "lua_manager_overlay.log", std::ios::app);
            if (file)
                file << GetTickCount64() << " " << owned << '\n';
        }
    }
    if constexpr (kEnableGameConsoleOutput) {
        const auto writer = g_game_console_write.load();
        if (!writer)
            return;
        static constexpr char source_path[] = "lua_manager_overlay.cpp";
        const char *source_name = source_path;
        const unsigned int source_line = 1;
        writer(14, 0x40000000u, source_path, source_line, &source_name, 0, &source_line, 0, owned.c_str());
        std::lock_guard lock(g_state.mutex);
        ++g_state.game_console_writes;
    }
    OutputDebugStringA((std::string("[lua_manager_overlay] ") + owned + "\n").c_str());
}

void record(ResolvedFunction &function, void *self) {
    {
        std::lock_guard lock(g_state.mutex);
        ++function.calls;
        function.last_this = reinterpret_cast<std::uintptr_t>(self);
        function.last_thread = GetCurrentThreadId();
        function.last_tick = GetTickCount64();
    }
}

HRESULT __stdcall hooked_present(IDXGISwapChain *swap_chain, UINT sync_interval, UINT flags);
HRESULT __stdcall hooked_resize_buffers(IDXGISwapChain *swap_chain, UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags);

void set_error(std::string message) {
    std::lock_guard lock(g_state.mutex);
    g_state.last_error = std::move(message);
}

bool readable(const void *address, std::size_t size) {
    return address && mem::ProcessMemory::readable(address, size);
}

bool safe_copy_from_game(const void *address, void *destination, std::size_t size) {
    if (!address || !destination || size == 0)
        return false;
    SIZE_T copied{};
    return ReadProcessMemory(GetCurrentProcess(), address, destination, size, &copied) != FALSE && copied == size;
}

template <typename T>
bool read_internal_field(const void *self, std::size_t offset, T &value) {
    const auto *base = static_cast<const std::uint8_t *>(self);
    if (!base || offset > static_cast<std::size_t>(-1) - sizeof(T))
        return false;
    return safe_copy_from_game(base + offset, &value, sizeof(T));
}

void snapshot_lua_manager_internals(void *self) {
    if constexpr (!kCaptureInternalState)
        return;
    LuaManagerInternalState snapshot{};
    snapshot.self = reinterpret_cast<std::uintptr_t>(self);
    if (!self)
        return;

    bool any = false;
    if (safe_copy_from_game(self, snapshot.raw_qwords.data(), 0x368)) {
        snapshot.raw_valid.fill(1);
        any = true;
    } else {
        for (std::size_t index = 0; index < snapshot.raw_qwords.size(); ++index) {
            if (read_internal_field(self, index * sizeof(std::uint64_t), snapshot.raw_qwords[index])) {
                snapshot.raw_valid[index] = 1;
                any = true;
            }
        }
    }
#define READ_LUA_FIELD(member, offset) any |= read_internal_field(self, offset, snapshot.member)
    READ_LUA_FIELD(callback_depth, 0x48);
    READ_LUA_FIELD(callback_index, 0x4C);
    READ_LUA_FIELD(callback_active, 0x58);
    READ_LUA_FIELD(callback_guard, 0x59);
    READ_LUA_FIELD(callback_kind, 0x5A);
    READ_LUA_FIELD(is_server, 0x354);
    READ_LUA_FIELD(callback_context_0x20, 0x20);
    READ_LUA_FIELD(callback_context_0x28, 0x28);
    READ_LUA_FIELD(callback_context_0x30, 0x30);
    READ_LUA_FIELD(callback_context_0x38, 0x38);
    READ_LUA_FIELD(callback_context_0x40, 0x40);
    READ_LUA_FIELD(callback_context_id, 0x4C);
    READ_LUA_FIELD(callback_context_type, 0x50);
    READ_LUA_FIELD(callback_context_flags, 0x54);
    READ_LUA_FIELD(container_0x0F8_begin, 0x0F8);
    READ_LUA_FIELD(container_0x100_end, 0x100);
    READ_LUA_FIELD(container_0x110_begin, 0x110);
    READ_LUA_FIELD(container_0x118_end, 0x118);
    READ_LUA_FIELD(container_0x150_begin, 0x150);
    READ_LUA_FIELD(container_0x158_end, 0x158);
    READ_LUA_FIELD(container_0x190_begin, 0x190);
    READ_LUA_FIELD(container_0x198_end, 0x198);
    READ_LUA_FIELD(container_0x1D0_begin, 0x1D0);
    READ_LUA_FIELD(container_0x1D8_end, 0x1D8);
    READ_LUA_FIELD(registry_storage, 0x218);
    READ_LUA_FIELD(registry_buckets, 0x228);
    READ_LUA_FIELD(registry_mask, 0x240);
    READ_LUA_FIELD(container_0x250, 0x250);
    READ_LUA_FIELD(container_0x258, 0x258);
    READ_LUA_FIELD(container_0x268, 0x268);
    READ_LUA_FIELD(container_mask_0x280, 0x280);
    READ_LUA_FIELD(container_0x308, 0x308);
    READ_LUA_FIELD(container_0x310, 0x310);
    READ_LUA_FIELD(container_0x320, 0x320);
    READ_LUA_FIELD(container_0x338, 0x338);
    READ_LUA_FIELD(server_callbacks_begin, 0x290);
    READ_LUA_FIELD(server_callbacks_end, 0x298);
    READ_LUA_FIELD(server_callbacks_capacity, 0x2A0);
    READ_LUA_FIELD(fixed_callbacks_begin, 0x2A8);
    READ_LUA_FIELD(fixed_callbacks_end, 0x2B0);
    READ_LUA_FIELD(fixed_callbacks_capacity, 0x2B8);
    READ_LUA_FIELD(client_fixed_begin, 0x2C0);
    READ_LUA_FIELD(client_fixed_end, 0x2C8);
    READ_LUA_FIELD(client_fixed_capacity, 0x2D0);
    READ_LUA_FIELD(client_update_begin, 0x2D8);
    READ_LUA_FIELD(client_update_end, 0x2E0);
    READ_LUA_FIELD(client_update_capacity, 0x2E8);
    READ_LUA_FIELD(receive_update_begin, 0x2F0);
    READ_LUA_FIELD(receive_update_end, 0x2F8);
    READ_LUA_FIELD(receive_update_capacity, 0x300);
    READ_LUA_FIELD(callback_count, 0x348);
    READ_LUA_FIELD(fixed_cursor, 0x350);
    READ_LUA_FIELD(lua_vm_shared_ptr, 0x358);
    READ_LUA_FIELD(lua_vm_control_block, 0x360);
#undef READ_LUA_FIELD
    snapshot.readable = any;
    std::lock_guard lock(g_state.mutex);
    g_state.internals = snapshot;
}

std::size_t callback_span(std::uintptr_t begin, std::uintptr_t end) {
    if (!begin || end < begin || end - begin > 0x100000)
        return 0;
    return static_cast<std::size_t>((end - begin) / sizeof(std::uintptr_t));
}

void snapshot_callback_vector(std::uintptr_t begin, std::uintptr_t end, CallbackVectorState &snapshot) {
    snapshot = {};
    snapshot.count = callback_span(begin, end);
    const auto shown = snapshot.count > snapshot.entries.size() ? snapshot.entries.size() : snapshot.count;
    for (std::size_t index = 0; index < shown; ++index) {
        std::uintptr_t entry_address{};
        if (!read_internal_field(reinterpret_cast<const void *>(begin), index * sizeof(std::uintptr_t), entry_address) ||
            !entry_address)
            continue;

        auto &entry = snapshot.entries[index];
        entry.address = entry_address;
        bool any = false;
#define READ_ENTRY_FIELD(member, offset) any |= read_internal_field(reinterpret_cast<const void *>(entry_address), offset, entry.member)
        READ_ENTRY_FIELD(field_0x10, 0x10);
        READ_ENTRY_FIELD(field_0x18, 0x18);
        READ_ENTRY_FIELD(field_0x20, 0x20);
        READ_ENTRY_FIELD(field_0x24, 0x24);
        READ_ENTRY_FIELD(field_0x28, 0x28);
        READ_ENTRY_FIELD(field_0x30, 0x30);
        READ_ENTRY_FIELD(field_0x38, 0x38);
#undef READ_ENTRY_FIELD
        entry.readable = any;
    }
}

void snapshot_callback_entries(void *self) {
    if constexpr (!kCaptureInternalState)
        return;
    if (!self)
        return;
    LuaManagerInternalState fields;
    {
        std::lock_guard lock(g_state.mutex);
        fields = g_state.internals;
    }
    CallbackVectorState server;
    CallbackVectorState fixed;
    CallbackVectorState client_fixed;
    CallbackVectorState client_update;
    CallbackVectorState receive_update;
    snapshot_callback_vector(fields.server_callbacks_begin, fields.server_callbacks_end, server);
    snapshot_callback_vector(fields.fixed_callbacks_begin, fields.fixed_callbacks_end, fixed);
    snapshot_callback_vector(fields.client_fixed_begin, fields.client_fixed_end, client_fixed);
    snapshot_callback_vector(fields.client_update_begin, fields.client_update_end, client_update);
    snapshot_callback_vector(fields.receive_update_begin, fields.receive_update_end, receive_update);
    std::lock_guard lock(g_state.mutex);
    g_state.internals.server_callback_entries = server;
    g_state.internals.fixed_callback_entries = fixed;
    g_state.internals.client_fixed_entries = client_fixed;
    g_state.internals.client_update_entries = client_update;
    g_state.internals.receive_update_entries = receive_update;
}

void snapshot_hash_container(void *self, std::size_t offset, bool byte_key, HashContainerState &snapshot) {
    snapshot = {};
    snapshot.object_offset = offset;
    if (!self)
        return;
    bool any = false;
    any |= read_internal_field(self, offset + 0x00, snapshot.max_load_factor);
    any |= read_internal_field(self, offset + 0x08, snapshot.head);
    any |= read_internal_field(self, offset + 0x10, snapshot.size);
    any |= read_internal_field(self, offset + 0x18, snapshot.buckets);
    any |= read_internal_field(self, offset + 0x30, snapshot.bucket_mask);
    any |= read_internal_field(self, offset + 0x38, snapshot.bucket_count);
    snapshot.readable = any;
    if (!snapshot.head)
        return;

    std::uintptr_t node{};
    if (!read_internal_field(reinterpret_cast<const void *>(snapshot.head), 0x08, node))
        return;
    for (auto &entry : snapshot.nodes) {
        if (!node || node == snapshot.head)
            break;
        bool duplicate = false;
        for (const auto &previous : snapshot.nodes) {
            if (previous.address == node) {
                duplicate = true;
                break;
            }
        }
        if (duplicate)
            break;
        entry.address = node;
        entry.readable = read_internal_field(reinterpret_cast<const void *>(node), 0x10, entry.key);
        if (byte_key) {
            std::uint8_t key{};
            entry.readable = read_internal_field(reinterpret_cast<const void *>(node), 0x10, key);
            entry.key = key;
        }
        read_internal_field(reinterpret_cast<const void *>(node), 0x18, entry.payload[0]);
        read_internal_field(reinterpret_cast<const void *>(node), 0x20, entry.payload[1]);
        read_internal_field(reinterpret_cast<const void *>(node), 0x28, entry.payload[2]);
        std::uintptr_t next{};
        if (!read_internal_field(reinterpret_cast<const void *>(node), 0x08, next))
            break;
        node = next;
    }
}

void snapshot_direct_lua_manager() {
    if constexpr (!kCaptureInternalState)
        return;
    if (!g_state.game)
        return;
    static ULONGLONG next_snapshot{};
    const auto now = GetTickCount64();
    if (now < next_snapshot)
        return;
    next_snapshot = now + 250;
    std::uintptr_t manager{};
    const auto slot = reinterpret_cast<const std::uint8_t *>(g_state.game) + kLuaManagerSingletonRva;
    if (!safe_copy_from_game(slot, &manager, sizeof(manager)) || !manager)
        return;
    snapshot_lua_manager_internals(reinterpret_cast<void *>(manager));
    snapshot_callback_entries(reinterpret_cast<void *>(manager));
    HashContainerState hashes;
    HashContainerState registry;
    snapshot_hash_container(reinterpret_cast<void *>(manager), 0x110, false, hashes);
    snapshot_hash_container(reinterpret_cast<void *>(manager), 0x210, true, registry);
    std::lock_guard lock(g_state.mutex);
    g_state.internals.registered_callback_hashes = hashes;
    g_state.internals.callback_type_registry = registry;
}

void update_script_identity(std::string_view path) {
    std::lock_guard lock(g_state.mutex);
    if (path.find("SurvivalGame.lua") != std::string_view::npos) {
        g_state.last_mode = "Survival";
        g_state.last_class = "SurvivalGame";
    } else if (path.find("ChallengeGame.lua") != std::string_view::npos) {
        g_state.last_mode = "Challenge";
        g_state.last_class = "ChallengeGame";
    } else if (path.find("MenuGame.lua") != std::string_view::npos) {
        g_state.last_mode = "Menu";
        g_state.last_class = "MenuGame";
    } else if (path.find("CreativeGame.lua") != std::string_view::npos) {
        g_state.last_mode = "Creative";
        g_state.last_class = "CreativeGame family";
    }
}

void update_mode_identity() {
    const auto mode_address = g_game_script_mode.load(std::memory_order_acquire);
    {
        std::lock_guard lock(g_state.mutex);
        g_state.mode_scalar_resolved = mode_address != nullptr;
        g_state.mode_scalar_address = reinterpret_cast<std::uintptr_t>(mode_address);
    }
    if (!readable(mode_address, sizeof(*mode_address)))
        return;
    const auto mode = *mode_address;
    const char *mode_name = nullptr;
    const char *class_name = nullptr;
    switch (mode) {
    case 14: mode_name = "Survival"; class_name = "SurvivalGame"; break;
    case 5: mode_name = "Challenge"; class_name = "ChallengeGame"; break;
    case 8: mode_name = "Menu"; class_name = "MenuGame"; break;
    case 0: mode_name = "Creative"; class_name = "CreativeGame"; break;
    case 1: mode_name = "Creative"; class_name = "CreativeFlatGame"; break;
    case 2: mode_name = "Creative"; class_name = "ClassicCreativeGame"; break;
    case 4:
    case 17: mode_name = "Creative"; class_name = "CreativeCustomGame"; break;
    case 7: mode_name = "Creative"; class_name = "CreativeTerrainGame"; break;
    default: break;
    }
    std::lock_guard lock(g_state.mutex);
    g_state.raw_mode_value = mode;
    if (mode_name) {
        g_state.last_mode = std::string(mode_name) + " (" + std::to_string(mode) + ")";
        g_state.last_class = class_name;
    } else {
        g_state.last_mode = "unknown (" + std::to_string(mode) + ")";
        g_state.last_class = "unknown";
    }
}

void draw_function(const ResolvedFunction &function) {
    ImGui::TextUnformatted(function.label);
    ImGui::SameLine(210.0f);
    ImGui::Text("%s  calls=%llu  last_this=%p  thread=%lu", function.resolved ? "resolved" : "missing",
        static_cast<unsigned long long>(function.calls), reinterpret_cast<void *>(function.last_this), function.last_thread);
    if (!function.resolved && !function.error.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("[%s]", function.error.c_str());
    }
}

void draw_callback_entries(const char *label, const CallbackVectorState &vector) {
    if (!ImGui::TreeNode(label))
        return;
    const auto shown = vector.count > vector.entries.size() ? vector.entries.size() : vector.count;
    ImGui::Text("entries=%zu  showing=%zu", vector.count, shown);
    for (std::size_t index = 0; index < shown; ++index) {
        const auto &entry = vector.entries[index];
        if (!entry.address) {
            ImGui::TextDisabled("[%zu] <unreadable entry pointer>", index);
            continue;
        }
        ImGui::Text("[%zu] %p  readable=%s  +30=%u  +38=%u", index,
            reinterpret_cast<void *>(entry.address), entry.readable ? "yes" : "no",
            entry.field_0x30, entry.field_0x38);
        ImGui::Text("     +10=0x%016llX  +18=0x%016llX  +20=%u  +24=%u  +28=%u",
            static_cast<unsigned long long>(entry.field_0x10), static_cast<unsigned long long>(entry.field_0x18),
            entry.field_0x20, entry.field_0x24, entry.field_0x28);
    }
    ImGui::TreePop();
}

void draw_hash_container(const char *label, const HashContainerState &container, bool byte_key) {
    if (!ImGui::TreeNode(label))
        return;
    ImGui::Text("object +0x%zX  readable=%s  size=%llu", container.object_offset,
        container.readable ? "yes" : "no", static_cast<unsigned long long>(container.size));
    ImGui::Text("max_load=%.3f  head=%p  buckets=%p  mask=0x%llX  bucket_count=%llu",
        container.max_load_factor, reinterpret_cast<void *>(container.head),
        reinterpret_cast<void *>(container.buckets), static_cast<unsigned long long>(container.bucket_mask),
        static_cast<unsigned long long>(container.bucket_count));
    for (std::size_t index = 0; index < container.nodes.size(); ++index) {
        const auto &node = container.nodes[index];
        if (!node.address)
            break;
        if (!node.readable) {
            ImGui::TextDisabled("[%zu] %p <unreadable>", index, reinterpret_cast<void *>(node.address));
            continue;
        }
        if (byte_key)
            ImGui::Text("[%zu] node=%p key(type)=0x%02llX payload=%p %p %p", index,
                reinterpret_cast<void *>(node.address), static_cast<unsigned long long>(node.key & 0xFF),
                reinterpret_cast<void *>(node.payload[0]), reinterpret_cast<void *>(node.payload[1]),
                reinterpret_cast<void *>(node.payload[2]));
        else
            ImGui::Text("[%zu] node=%p hash=0x%016llX", index, reinterpret_cast<void *>(node.address),
                static_cast<unsigned long long>(node.key));
    }
    ImGui::TreePop();
}

void draw_overlay() {
    bool visible;
    {
        std::lock_guard lock(g_state.mutex);
        visible = g_state.visible;
    }
    if (!visible)
        return;

    update_mode_identity();

    ImGui::SetNextWindowSize(ImVec2(980.0f, 760.0f), ImGuiCond_FirstUseEver);
    ImGui::Begin("Scrap Mechanic LuaManager", &g_state.visible, ImGuiWindowFlags_NoCollapse);
    ImGui::Text("LuaManager");
    ImGui::Separator();
    if (ImGui::CollapsingHeader("Runtime and manager state", ImGuiTreeNodeFlags_DefaultOpen)) {
        std::lock_guard lock(g_state.mutex);
        ImGui::Text("Game module: %p", g_state.game);
        ImGui::Text("UTILS::Console logger: %s  writes=%llu", g_state.game_console_resolved ? "resolved" : "missing",
            static_cast<unsigned long long>(g_state.game_console_writes));
        ImGui::Text("Mode: %s    class: %s", g_state.last_mode.empty() ? "unknown" : g_state.last_mode.c_str(),
            g_state.last_class.empty() ? "unknown" : g_state.last_class.c_str());
        ImGui::Text("Mode scalar: %s  raw=%d  address=%p", g_state.mode_scalar_resolved ? "resolved" : "missing",
            static_cast<int>(g_state.raw_mode_value), reinterpret_cast<void *>(g_state.mode_scalar_address));
        ImGui::Text("Client detour phase: %lu", static_cast<unsigned long>(g_client_detour_phase.load(std::memory_order_relaxed)));
        if (!g_state.last_error.empty())
            ImGui::TextColored(ImVec4(1, .45f, .3f, 1), "Resolver: %s", g_state.last_error.c_str());
        ImGui::Separator();
        ImGui::Text("LuaManager internal fields (current IDB layout)");
        const auto &fields = g_state.internals;
        ImGui::Text("self=%p  readable=%s", reinterpret_cast<void *>(fields.self), fields.readable ? "yes" : "no");
        ImGui::Text("manager: role=%s  callback: depth=%d  index=%lld  active=%u  guard=%u  kind=%u",
            fields.is_server ? "server" : "client", fields.callback_depth, static_cast<long long>(fields.callback_index),
            fields.callback_active, fields.callback_guard, fields.callback_kind);
        ImGui::Text("callback context: id=%u  type=%u  flags=0x%08X", fields.callback_context_id,
            fields.callback_context_type, fields.callback_context_flags);
        ImGui::Text("context slots: %p  %p  %p  %p  %p", reinterpret_cast<void *>(fields.callback_context_0x20),
            reinterpret_cast<void *>(fields.callback_context_0x28), reinterpret_cast<void *>(fields.callback_context_0x30),
            reinterpret_cast<void *>(fields.callback_context_0x38), reinterpret_cast<void *>(fields.callback_context_0x40));
        ImGui::Text("counters: create_instances=%u  fixed_cursor=%u", fields.callback_count, fields.fixed_cursor);
        ImGui::Text("script containers: 0x0F8=%p..%p  0x110=%p..%p  0x150=%p..%p",
            reinterpret_cast<void *>(fields.container_0x0F8_begin), reinterpret_cast<void *>(fields.container_0x100_end),
            reinterpret_cast<void *>(fields.container_0x110_begin), reinterpret_cast<void *>(fields.container_0x118_end),
            reinterpret_cast<void *>(fields.container_0x150_begin), reinterpret_cast<void *>(fields.container_0x158_end));
        ImGui::Text("script containers: 0x190=%p..%p  0x1D0=%p..%p",
            reinterpret_cast<void *>(fields.container_0x190_begin), reinterpret_cast<void *>(fields.container_0x198_end),
            reinterpret_cast<void *>(fields.container_0x1D0_begin), reinterpret_cast<void *>(fields.container_0x1D8_end));
        ImGui::Text("registry raw: storage=%p  buckets=%p  mask/meta=%p",
            reinterpret_cast<void *>(fields.registry_storage), reinterpret_cast<void *>(fields.registry_buckets),
            reinterpret_cast<void *>(fields.registry_mask));
        ImGui::Text("other raw slots: +250=%p  +258=%p  +268=%p  mask +280=%p",
            reinterpret_cast<void *>(fields.container_0x250), reinterpret_cast<void *>(fields.container_0x258),
            reinterpret_cast<void *>(fields.container_0x268), reinterpret_cast<void *>(fields.container_mask_0x280));
        ImGui::Text("other raw slots: +308=%p  +310=%p  +320=%p  +338=%p",
            reinterpret_cast<void *>(fields.container_0x308), reinterpret_cast<void *>(fields.container_0x310),
            reinterpret_cast<void *>(fields.container_0x320), reinterpret_cast<void *>(fields.container_0x338));
        ImGui::Text("server callbacks: %zu  begin=%p end=%p cap=%p",
            callback_span(fields.server_callbacks_begin, fields.server_callbacks_end),
            reinterpret_cast<void *>(fields.server_callbacks_begin), reinterpret_cast<void *>(fields.server_callbacks_end),
            reinterpret_cast<void *>(fields.server_callbacks_capacity));
        ImGui::Text("fixed callbacks: %zu  begin=%p end=%p cap=%p",
            callback_span(fields.fixed_callbacks_begin, fields.fixed_callbacks_end),
            reinterpret_cast<void *>(fields.fixed_callbacks_begin), reinterpret_cast<void *>(fields.fixed_callbacks_end),
            reinterpret_cast<void *>(fields.fixed_callbacks_capacity));
        ImGui::Text("client fixed: %zu  begin=%p end=%p cap=%p",
            callback_span(fields.client_fixed_begin, fields.client_fixed_end),
            reinterpret_cast<void *>(fields.client_fixed_begin), reinterpret_cast<void *>(fields.client_fixed_end),
            reinterpret_cast<void *>(fields.client_fixed_capacity));
        ImGui::Text("client update: %zu  begin=%p end=%p cap=%p",
            callback_span(fields.client_update_begin, fields.client_update_end),
            reinterpret_cast<void *>(fields.client_update_begin), reinterpret_cast<void *>(fields.client_update_end),
            reinterpret_cast<void *>(fields.client_update_capacity));
        ImGui::Text("receive update: %zu  begin=%p end=%p cap=%p",
            callback_span(fields.receive_update_begin, fields.receive_update_end),
            reinterpret_cast<void *>(fields.receive_update_begin), reinterpret_cast<void *>(fields.receive_update_end),
            reinterpret_cast<void *>(fields.receive_update_capacity));
        ImGui::Text("LuaVM shared_ptr: object=%p  control=%p",
            reinterpret_cast<void *>(fields.lua_vm_shared_ptr), reinterpret_cast<void *>(fields.lua_vm_control_block));
        if (ImGui::CollapsingHeader("Reverse-engineered containers")) {
            draw_hash_container("registered callback hashes (+0x110)", fields.registered_callback_hashes, false);
            draw_hash_container("callback type registry (+0x210)", fields.callback_type_registry, true);
        }
        if (ImGui::CollapsingHeader("Callback object entries (guarded samples)")) {
            draw_callback_entries("server callback objects", fields.server_callback_entries);
            draw_callback_entries("fixed callback objects", fields.fixed_callback_entries);
            draw_callback_entries("client-fixed callback objects", fields.client_fixed_entries);
            draw_callback_entries("client-update callback objects", fields.client_update_entries);
            draw_callback_entries("receive-update callback objects", fields.receive_update_entries);
        }
        if (ImGui::CollapsingHeader("Raw LuaManager slots (+0x000..+0x367)")) {
            ImGui::BeginChild("LuaManagerRawSlots", ImVec2(0.0f, 190.0f), true);
            for (std::size_t index = 0; index < fields.raw_qwords.size(); ++index) {
                if (fields.raw_valid[index])
                    ImGui::Text("+0x%03zX  0x%016llX", index * sizeof(std::uint64_t),
                        static_cast<unsigned long long>(fields.raw_qwords[index]));
                else
                    ImGui::TextDisabled("+0x%03zX  <unreadable>", index * sizeof(std::uint64_t));
            }
            ImGui::EndChild();
        }
    }
    ImGui::Separator();
    ImGui::End();
}

bool is_game_input_message(UINT message) {
    switch (message) {
    case WM_KEYDOWN: case WM_KEYUP: case WM_CHAR: case WM_SYSKEYDOWN: case WM_SYSKEYUP:
    case WM_SYSCHAR: case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK: case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK: case WM_XBUTTONDOWN:
    case WM_XBUTTONUP: case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
        return true;
    default:
        return false;
    }
}

LRESULT CALLBACK hooked_wndproc(HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
    bool menu_toggle_message = false;
    if (message == WM_KEYDOWN && w_param == VK_INSERT && !g_state.menu_key_down) {
        menu_toggle_message = true;
        g_state.menu_key_down = true;
        std::lock_guard lock(g_state.mutex);
        g_state.visible = !g_state.visible;
    } else if (message == WM_KEYUP && w_param == VK_INSERT) {
        menu_toggle_message = true;
        g_state.menu_key_down = false;
    }
    const auto imgui_handled = ImGui_ImplWin32_WndProcHandler(window, message, w_param, l_param) != 0;
    bool menu_open{};
    {
        std::lock_guard lock(g_state.mutex);
        menu_open = g_state.visible;
    }
    // ImGui receives every message first. While the menu is open, consume
    // input messages so the game never receives the same keyboard/mouse event.
    if (menu_toggle_message || (menu_open && (imgui_handled || is_game_input_message(message))))
        return 0;
    return g_state.original_wndproc ? CallWindowProcW(g_state.original_wndproc, window, message, w_param, l_param)
        : DefWindowProcW(window, message, w_param, l_param);
}

void destroy_imgui() {
    if (g_state.original_wndproc && g_state.window) {
        SetWindowLongPtrW(g_state.window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_state.original_wndproc));
        g_state.original_wndproc = nullptr;
    }
    if (g_state.render_target) { g_state.render_target->Release(); g_state.render_target = nullptr; }
    if (g_state.context) { g_state.context->Release(); g_state.context = nullptr; }
    if (g_state.device) { g_state.device->Release(); g_state.device = nullptr; }
    g_state.imgui_ready = false;
    if (ImGui::GetCurrentContext()) {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    }
}

bool create_render_target(IDXGISwapChain *swap_chain) {
    ID3D11Texture2D *back_buffer{};
    if (FAILED(swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer))))
        return false;
    const auto result = g_state.device->CreateRenderTargetView(back_buffer, nullptr, &g_state.render_target);
    back_buffer->Release();
    return SUCCEEDED(result);
}

bool initialize_imgui(IDXGISwapChain *swap_chain) {
    if (g_state.imgui_ready)
        return true;
    if (FAILED(swap_chain->GetDevice(IID_PPV_ARGS(&g_state.device))))
        return false;
    g_state.device->GetImmediateContext(&g_state.context);
    if (!create_render_target(swap_chain))
        return false;

    DXGI_SWAP_CHAIN_DESC description{};
    if (FAILED(swap_chain->GetDesc(&description)))
        return false;
    g_state.window = description.OutputWindow;
    if (g_state.window)
        g_state.original_wndproc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(g_state.window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(hooked_wndproc)));

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    if (!ImGui_ImplWin32_Init(g_state.window) || !ImGui_ImplDX11_Init(g_state.device, g_state.context)) {
        destroy_imgui();
        return false;
    }
    auto &io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(description.BufferDesc.Width), static_cast<float>(description.BufferDesc.Height));
    g_state.imgui_ready = true;
    log_line("D3D11 overlay initialized; Steam overlay remains in the compositor chain");
    return true;
}

HRESULT __stdcall hooked_resize_buffers(IDXGISwapChain *swap_chain, UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags) {
    g_present_in_flight.fetch_add(1, std::memory_order_acq_rel);
    struct ResizeGuard {
        ~ResizeGuard() { g_present_in_flight.fetch_sub(1, std::memory_order_release); }
    } resize_guard;
    const auto original = g_resize_hook.original();
    if (!original)
        return E_FAIL;
    if (g_unload_requested.load(std::memory_order_acquire))
        return original(swap_chain, count, width, height, format, flags);
    destroy_imgui();
    return original(swap_chain, count, width, height, format, flags);
}

HRESULT __stdcall hooked_present(IDXGISwapChain *swap_chain, UINT sync_interval, UINT flags) {
    g_present_in_flight.fetch_add(1, std::memory_order_acq_rel);
    struct PresentGuard {
        ~PresentGuard() { g_present_in_flight.fetch_sub(1, std::memory_order_release); }
    } present_guard;
    const auto original = g_present_hook.original();
    if (!original)
        return E_FAIL;
    if (g_unload_requested.load(std::memory_order_acquire))
        return original(swap_chain, sync_interval, flags);
    if (initialize_imgui(swap_chain)) {
        auto &io = ImGui::GetIO();
        DXGI_SWAP_CHAIN_DESC description{};
        if (SUCCEEDED(swap_chain->GetDesc(&description))) {
            RECT client{};
            GetClientRect(description.OutputWindow, &client);
            io.DisplaySize = ImVec2(static_cast<float>(client.right - client.left), static_cast<float>(client.bottom - client.top));
        }
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        snapshot_direct_lua_manager();
        draw_overlay();
        ImGui::Render();
        g_state.context->OMSetRenderTargets(1, &g_state.render_target, nullptr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        if (!g_overlay_frame_marked.exchange(true, std::memory_order_acq_rel))
            write_overlay_frame_marker();
    }
    return original(swap_chain, sync_interval, flags);
}

bool install_present_hooks() {
    WNDCLASSEXW window_class{sizeof(WNDCLASSEXW), CS_HREDRAW | CS_VREDRAW, DefWindowProcW, 0, 0, GetModuleHandleW(nullptr), nullptr, nullptr, nullptr, nullptr, L"LuaManagerOverlayProbe", nullptr};
    RegisterClassExW(&window_class);
    HWND window = CreateWindowW(window_class.lpszClassName, L"LuaManagerOverlayProbe", WS_OVERLAPPEDWINDOW, 0, 0, 1, 1, nullptr, nullptr, window_class.hInstance, nullptr);
    if (!window)
        return false;
    DXGI_SWAP_CHAIN_DESC description{};
    description.BufferCount = 1;
    description.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.OutputWindow = window;
    description.SampleDesc.Count = 1;
    description.Windowed = TRUE;
    IDXGISwapChain *swap_chain{};
    ID3D11Device *device{};
    ID3D11DeviceContext *context{};
    const auto result = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
        &description, &swap_chain, &device, nullptr, &context);
    if (context) context->Release();
    if (device) device->Release();
    if (FAILED(result) || !swap_chain) {
        DestroyWindow(window);
        return false;
    }
    auto **vtable = *reinterpret_cast<void ***>(swap_chain);
    const auto present = reinterpret_cast<PresentFn>(vtable[8]);
    const auto resize = reinterpret_cast<ResizeBuffersFn>(vtable[13]);
    const auto present_result = g_present_hook.install(present, hooked_present);
    const auto resize_result = g_resize_hook.install(resize, hooked_resize_buffers);
    swap_chain->Release();
    DestroyWindow(window);
    return static_cast<bool>(present_result) && static_cast<bool>(resize_result);
}

void remove_all_hooks() {
    log_line("unload: disabling hooks");
    g_resize_hook.remove();
    g_present_hook.remove();
    mem::hook::uninitialize();
    log_line("unload: hooks removed");
}

void cleanup_for_unload() {
    if (g_cleanup_done.exchange(true, std::memory_order_acq_rel))
        return;
    log_line("unload: requested");
    g_unload_requested.store(true, std::memory_order_release);
    g_state.stopping.store(true, std::memory_order_release);
    g_present_hook.disable();
    g_resize_hook.disable();
    while (!g_bootstrap_finished.load(std::memory_order_acquire) || g_present_in_flight.load(std::memory_order_acquire) != 0)
        Sleep(1);
    destroy_imgui();
    remove_all_hooks();
    if (g_crash_handler) {
        RemoveVectoredExceptionHandler(g_crash_handler);
        g_crash_handler = nullptr;
    }
    log_line("unload: cleanup complete");
}

bool resolve_game_console(mem::Scan &scan) {
    auto first_anchor = scan.string_xref("Unable to allocate debug console", "UTILS::Console allocation xref");
    auto second_anchor = scan.string_xref("Unable to setup debug console", "UTILS::Console setup xref");
    auto first_message = scan.resolver().unique_string("Unable to allocate debug console", "UTILS::Console allocation diagnostic");
    auto second_message = scan.resolver().unique_string("Unable to setup debug console", "UTILS::Console setup diagnostic");
    if (!first_anchor || !second_anchor || !first_message || !second_message) {
        set_error("UTILS::Console diagnostic strings could not be resolved");
        return false;
    }
    auto first_function = scan.containing_function(first_anchor.get(), "UTILS::Console allocation function");
    auto second_function = scan.containing_function(second_anchor.get(), "UTILS::Console setup function");
    if (!first_message || !second_message || !first_function || !second_function || first_function.get() != second_function.get()) {
        set_error("UTILS::Console diagnostics could not be resolved");
        return false;
    }

    auto range = scan.function_range(first_function.get(), "UTILS::Console diagnostic function range");
    if (!range)
        return false;

    GameConsoleWriteFn candidate{};
    for (const auto &site : mem::call_sites(range.get())) {
        bool references_message = false;
        for (const auto &instruction : site.preceding_instructions) {
            const auto referenced = mem::resolve_rip_target(instruction.address);
            if (referenced == first_message.get() || referenced == second_message.get()) {
                references_message = true;
                break;
            }
        }
        if (!references_message || !site.target())
            continue;
        auto target = reinterpret_cast<GameConsoleWriteFn>(const_cast<std::uint8_t *>(site.target()));
        if (!candidate)
            candidate = target;
        else if (candidate != target) {
            set_error("UTILS::Console logger resolution was ambiguous");
            return false;
        }
    }
    if (!candidate) {
        set_error("UTILS::Console logger call was not found");
        return false;
    }
    g_game_console_write.store(candidate);
    {
        std::lock_guard lock(g_state.mutex);
        g_state.game_console_resolved = true;
    }
    return true;
}

template <typename Fn>
bool resolve_and_hook(mem::Scan &scan, ResolvedFunction &description, Fn detour, mem::hook::Function<Fn> &hook) {
    auto anchor = scan.string_xref(description.anchor, description.label);
    if (!anchor) {
        description.error = anchor.error.message;
        return false;
    }
    auto function = scan.containing_function(anchor.get(), description.label);
    if (!function) {
        description.error = function.error.message;
        return false;
    }
    description.address = const_cast<std::uint8_t *>(function.get());
    // Do not suspend every game thread while installing a live dispatcher
    // hook. Some LuaManager dispatchers are continuously active during game
    // startup; suspending a thread that owns a game/runtime lock can leave
    // the process permanently nonresponsive before the trampoline is ready.
    auto result = hook.install(reinterpret_cast<Fn>(description.address), detour);
    description.resolved = static_cast<bool>(result);
    if (!result) {
        description.error = result.error.message;
        log_line(std::string("hook failed: ") + description.label + ": " + description.error);
    } else {
        log_line(std::string("hook installed: ") + description.label);
    }
    return static_cast<bool>(result);
}

bool resolve_game_script_mode(mem::Scan &scan, const void *script_function) {
    auto range = scan.function_range(reinterpret_cast<const std::uint8_t *>(script_function), "GameScript mode function range");
    if (!range)
        return false;
    auto matches = scan.find(range.get(), "8B 05 ?? ?? ?? ?? 83 F8 0E");
    if (!matches || matches->size() != 1) {
        log_line("GameScript mode scalar: signature was not unique");
        return false;
    }
    // This is the six-byte 8B 05 disp32 form; memorylib's generic helper
    // intentionally handles the seven-byte forms used by its xref scanner.
    std::int32_t displacement{};
    std::memcpy(&displacement, matches->front() + 2, sizeof(displacement));
    const auto target = reinterpret_cast<const std::uint8_t *>(reinterpret_cast<std::uintptr_t>(matches->front() + 6) + displacement);
    if (!readable(target, sizeof(std::int32_t))) {
        log_line("GameScript mode scalar: resolved address was unreadable");
        return false;
    }
    g_game_script_mode.store(reinterpret_cast<const std::int32_t *>(target), std::memory_order_release);
    log_line("GameScript mode scalar resolved");
    return true;
}

void *__fastcall hooked_initialize(void *self, void *argument) {
    record(g_state.initialize, self);
    const auto original = g_initialize_hook.original();
    if (!original) { loader_marker("initialize original trampoline missing"); return nullptr; }
    return original(self, argument);
}
void *__fastcall hooked_script_load(void *self, void *argument) {
    record(g_state.script_load, self);
    const auto original = g_script_load_hook.original();
    if (!original) { loader_marker("script-load original trampoline missing"); return nullptr; }
    return original(self, argument);
}
std::intptr_t __fastcall hooked_client_update(void *self, float delta_time) {
    record(g_state.client_update, self);
    // Do not scan the manager from the per-frame path; lifecycle/VM hooks
    // refresh the detailed snapshot without stalling the game loop.
    g_client_detour_phase.store(1, std::memory_order_relaxed);
    g_client_detour_phase.store(2, std::memory_order_relaxed);
    if constexpr (kProbeClientHookWithoutOriginal)
        return 0;
    const auto original = g_client_update_hook.original();
    if (!original) {
        loader_marker("client update original trampoline missing");
        return 0;
    }
    return original(self, delta_time);
}
char *__fastcall hooked_fixed_update(void *self) {
    record(g_state.fixed_update, self);
    // Fixed-update is also hot; keep this detour record/forward-only.
    const auto original = g_fixed_update_hook.original();
    if (!original) { loader_marker("fixed-update original trampoline missing"); return nullptr; }
    return original(self);
}
std::intptr_t __fastcall hooked_vm_refresh(void *self, std::intptr_t argument) {
    record(g_state.vm_refresh, self);
    const auto original = g_vm_refresh_hook.original();
    if (!original) { loader_marker("VM-refresh original trampoline missing"); return 0; }
    return original(self, argument);
}
std::intptr_t __fastcall hooked_client_data(std::intptr_t a1, std::intptr_t a2, std::intptr_t a3, std::intptr_t a4,
    std::intptr_t *a5, int a6, unsigned int *a7, int a8, int a9, void *a10) {
    record(g_state.client_data, reinterpret_cast<void *>(a1));
    const auto original = g_client_data_hook.original();
    if (!original) { loader_marker("client-data original trampoline missing"); return 0; }
    return original(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10);
}
char __fastcall hooked_lifecycle(void *self, std::intptr_t argument, std::uint8_t kind, int flags) {
    record(g_state.lifecycle, self);
    {
        std::lock_guard lock(g_state.mutex);
        g_state.last_lifecycle_argument = argument;
        g_state.last_lifecycle_kind = kind;
        g_state.last_lifecycle_flags = flags;
    }
    const auto original = g_lifecycle_hook.original();
    if (!original) { loader_marker("lifecycle original trampoline missing"); return 0; }
    return original(self, argument, kind, flags);
}

DWORD WINAPI bootstrap(void *) {
    struct BootstrapCompletion {
        ~BootstrapCompletion() { g_bootstrap_finished.store(true, std::memory_order_release); }
    } completion;
    loader_marker("bootstrap entered");
    g_state.game = GetModuleHandleW(L"ScrapMechanic.exe");
    for (unsigned attempt = 0; attempt < 120 && !g_state.stopping.load(); ++attempt) {
        if (!g_state.game)
            g_state.game = GetModuleHandleW(L"ScrapMechanic.exe");
        auto scan_result = mem::Scan::open(L"ScrapMechanic.exe", [](const mem::Diagnostic &diagnostic) {
            log_line(diagnostic.stage + ": " + diagnostic.message);
        });
        if (!scan_result) { Sleep(1000); continue; }
        auto &scan = scan_result.get();
        mem::hook::initialize();
        if (!install_present_hooks())
            set_error("DXGI Present/ResizeBuffers hook installation failed");
        log_line("LuaManager detours disabled; direct singleton structure view active");
        log_line("D3D11 overlay and window-message integration initialized");
        write_ready_marker();
        return 0;
    }
    set_error("Scrap Mechanic module did not become available");
    return 0;
}

extern "C" __declspec(dllexport) DWORD WINAPI LuaManagerOverlay_Unload(HMODULE module) {
    cleanup_for_unload();
    log_line("unload: releasing module");
    FreeLibraryAndExitThread(module ? module : g_self_module, 0);
    return 0;
}

extern "C" BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self_module = module;
        loader_marker("DLL_PROCESS_ATTACH");
        DisableThreadLibraryCalls(module);
        // Register last in the VEH chain. The game remains authoritative for
        // exception handling; this handler only writes a dump if handling
        // continues past the game's own handlers.
        g_crash_handler = AddVectoredExceptionHandler(FALSE, crash_report_vectored_handler);
        loader_marker(g_crash_handler ? "crash reporter registered after game VEH" : "crash reporter registration failed");
        if (auto thread = CreateThread(nullptr, 0, bootstrap, nullptr, 0, nullptr))
            CloseHandle(thread);
    } else if (reason == DLL_PROCESS_DETACH) {
        g_state.stopping.store(true);
    }
    return TRUE;
}

} // namespace
