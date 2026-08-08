#include <windows.h>

#include "memorylib/memorylib.hpp"

#include <cstdint>
#include <cstring>

namespace {

constexpr std::size_t kPatchSize = 6;
constexpr char kPhysicsHungString[] = "Physics hung!";
constexpr char kFallbackPattern[] = "41 83 FF 08 0F 85 ?? ?? ?? ??";

struct PatchState {
    std::uint8_t *address{};
    std::uint8_t original[kPatchSize]{};
    bool applied{};
};

PatchState g_patch;

void report(const mem::Diagnostic &diagnostic) {
    char message[512]{};
    wsprintfA(message, "[no_smart_physics] %s: %s (0x%p)\n", diagnostic.stage.c_str(), diagnostic.message.c_str(), reinterpret_cast<void *>(diagnostic.address));
    OutputDebugStringA(message);
}

bool apply_patch() {
    if (g_patch.applied) {
        return true;
    }

    auto scan_result = mem::Scan::open(L"ScrapMechanic.exe", report);
    if (!scan_result)
        return false;
    auto &scan = scan_result.get();
    const auto &sections = scan.sections();

    // The diagnostic string is emitted by the same GameInstance function as
    // the Advanced -> Smart fallback. This survives ASLR and relocations.
    const auto string_xref = scan.string_xref(kPhysicsHungString, "physics diagnostic string");
    if (!string_xref) {
        return false;
    }

    const auto function = scan.resolver().containing_function(string_xref.get(), "GameInstance physics function");
    if (!function) {
        return false;
    }

    const auto bounds = scan.function_range(function.get(), "GameInstance physics function");
    if (!bounds) {
        report(bounds.error);
        return false;
    }

    const auto matches_result = scan.find(*bounds, kFallbackPattern);
    if (!matches_result) {
        report(matches_result.error);
        return false;
    }
    const auto match = mem::exactly_one(matches_result.get(), "fallback branch pattern");
    if (!match) {
        report(match.error);
        return false;
    }

    // Match layout: cmp r15d, 8 (4 bytes), followed by jnz rel32 (6 bytes).
    auto *branch = const_cast<std::uint8_t *>(match.get() + 4);
    if (branch[0] != 0x0F || branch[1] != 0x85) {
        return false;
    }

    std::int32_t old_relative = 0;
    std::memcpy(&old_relative, branch + 2, sizeof(old_relative));
    auto *destination = branch + kPatchSize + old_relative;
    if (!sections.text.contains(destination)) {
        return false;
    }

    // Preserve the original destination, but make the branch unconditional.
    const auto relative_jump = static_cast<std::int32_t>(reinterpret_cast<std::uintptr_t>(destination) - (reinterpret_cast<std::uintptr_t>(branch) + 5));
    std::uint8_t replacement[kPatchSize] = {0xE9, static_cast<std::uint8_t>(relative_jump), static_cast<std::uint8_t>(relative_jump >> 8),
        static_cast<std::uint8_t>(relative_jump >> 16), static_cast<std::uint8_t>(relative_jump >> 24), 0x90};

    std::memcpy(g_patch.original, branch, kPatchSize);
    DWORD old_protection = 0;
    if (!VirtualProtect(branch, kPatchSize, PAGE_EXECUTE_READWRITE, &old_protection)) {
        return false;
    }

    std::memcpy(branch, replacement, kPatchSize);
    FlushInstructionCache(GetCurrentProcess(), branch, kPatchSize);

    DWORD ignored = 0;
    VirtualProtect(branch, kPatchSize, old_protection, &ignored);
    g_patch.address = branch;
    g_patch.applied = true;
    return true;
}

void remove_patch() {
    if (!g_patch.applied || !g_patch.address) {
        return;
    }

    DWORD old_protection = 0;
    if (VirtualProtect(g_patch.address, kPatchSize, PAGE_EXECUTE_READWRITE, &old_protection)) {
        std::memcpy(g_patch.address, g_patch.original, kPatchSize);
        FlushInstructionCache(GetCurrentProcess(), g_patch.address, kPatchSize);
        DWORD ignored = 0;
        VirtualProtect(g_patch.address, kPatchSize, old_protection, &ignored);
    }

    g_patch = {};
}

DWORD WINAPI bootstrap(void *) {
    apply_patch();
    return 0;
}

} // namespace

extern "C" __declspec(dllexport) BOOL NoSmartPhysics_Install() {
    return apply_patch() ? TRUE : FALSE;
}

extern "C" __declspec(dllexport) void NoSmartPhysics_Remove() {
    remove_patch();
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        if (auto thread = CreateThread(nullptr, 0, bootstrap, nullptr, 0, nullptr)) {
            CloseHandle(thread);
        }
    }
    return TRUE;
}
