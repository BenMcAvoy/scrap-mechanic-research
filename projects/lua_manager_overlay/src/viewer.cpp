#include <windows.h>
#include <tlhelp32.h>

#include <array>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr std::uintptr_t kImageBase = 0x140000000ull;
constexpr std::uintptr_t kLuaManagerSingleton = 0x141AA25A8ull;
constexpr std::size_t kManagerSnapshotSize = 0x368;
constexpr UINT kRefreshTimer = 1;

struct Field {
    std::size_t offset;
    const wchar_t *name;
};

constexpr std::array kFields{
    Field{0x20, L"callback context +0x20"},
    Field{0x28, L"callback context +0x28"},
    Field{0x30, L"callback context +0x30"},
    Field{0x38, L"callback context +0x38"},
    Field{0x40, L"callback context +0x40"},
    Field{0x48, L"callback depth"},
    Field{0x4C, L"callback index / context id"},
    Field{0x50, L"callback context type"},
    Field{0x54, L"callback context flags"},
    Field{0x58, L"callback active"},
    Field{0x59, L"callback guard"},
    Field{0x5A, L"callback kind"},
    Field{0x0F8, L"container A begin"},
    Field{0x100, L"container A end"},
    Field{0x110, L"container B begin"},
    Field{0x118, L"container B end"},
    Field{0x150, L"container C begin"},
    Field{0x158, L"container C end"},
    Field{0x190, L"container D begin"},
    Field{0x198, L"container D end"},
    Field{0x1D0, L"container E begin"},
    Field{0x1D8, L"container E end"},
    Field{0x218, L"registry storage"},
    Field{0x228, L"registry buckets"},
    Field{0x240, L"registry mask"},
    Field{0x250, L"container +0x250"},
    Field{0x258, L"container +0x258"},
    Field{0x268, L"container +0x268"},
    Field{0x280, L"container mask"},
    Field{0x290, L"server callbacks begin"},
    Field{0x298, L"server callbacks end"},
    Field{0x2A0, L"server callbacks capacity"},
    Field{0x2A8, L"fixed callbacks begin"},
    Field{0x2B0, L"fixed callbacks end"},
    Field{0x2B8, L"fixed callbacks capacity"},
    Field{0x2C0, L"client fixed begin"},
    Field{0x2C8, L"client fixed end"},
    Field{0x2D0, L"client fixed capacity"},
    Field{0x2D8, L"client update begin"},
    Field{0x2E0, L"client update end"},
    Field{0x2E8, L"client update capacity"},
    Field{0x2F0, L"receive update begin"},
    Field{0x2F8, L"receive update end"},
    Field{0x300, L"receive update capacity"},
    Field{0x308, L"container +0x308"},
    Field{0x310, L"container +0x310"},
    Field{0x320, L"container +0x320"},
    Field{0x338, L"container +0x338"},
    Field{0x348, L"callback count"},
    Field{0x350, L"fixed cursor"},
    Field{0x354, L"server/client role byte"},
    Field{0x358, L"LuaVM shared object"},
    Field{0x360, L"LuaVM control block"},
};

HWND g_output{};
HWND g_auto_refresh{};
HANDLE g_process{};
DWORD g_pid{};

std::wstring hex(std::uintptr_t value, unsigned width = 0) {
    std::wstringstream stream;
    stream << L"0x" << std::uppercase << std::hex << std::setfill(L'0') << std::setw(width) << value;
    return stream.str();
}

DWORD find_process() {
    const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return 0;
    PROCESSENTRY32W entry{sizeof(entry)};
    DWORD result{};
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, L"ScrapMechanic.exe") == 0) {
                result = entry.th32ProcessID;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return result;
}

std::uintptr_t module_base(DWORD pid) {
    const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snapshot == INVALID_HANDLE_VALUE)
        return 0;
    MODULEENTRY32W entry{sizeof(entry)};
    std::uintptr_t result{};
    if (Module32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szModule, L"ScrapMechanic.exe") == 0) {
                result = reinterpret_cast<std::uintptr_t>(entry.modBaseAddr);
                break;
            }
        } while (Module32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return result;
}

bool read_memory(std::uintptr_t address, void *data, std::size_t size) {
    SIZE_T copied{};
    return g_process && ReadProcessMemory(g_process, reinterpret_cast<const void *>(address), data, size, &copied) && copied == size;
}

void append(std::wstring &text, const std::wstring &line) {
    text += line;
    text += L"\r\n";
}

void refresh_view() {
    if (g_process) {
        CloseHandle(g_process);
        g_process = nullptr;
    }
    g_pid = find_process();
    if (!g_pid) {
        SetWindowTextW(g_output, L"ScrapMechanic.exe is not running.\r\n");
        return;
    }
    g_process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, g_pid);
    const auto base = module_base(g_pid);
    std::wstring text;
    append(text, L"External LuaManager structure viewer");
    append(text, L"No hooks or injected memory reads are used.");
    append(text, L"PID: " + std::to_wstring(g_pid));
    append(text, L"Module base: " + hex(base, 16));
    append(text, L"Singleton slot RVA: " + hex(kLuaManagerSingleton - kImageBase, 8));
    if (!base || !g_process) {
        append(text, L"Unable to open the game or resolve its module.");
        SetWindowTextW(g_output, text.c_str());
        return;
    }

    const auto singleton_slot = base + (kLuaManagerSingleton - kImageBase);
    std::uintptr_t manager{};
    if (!read_memory(singleton_slot, &manager, sizeof(manager)) || !manager) {
        append(text, L"LuaManager singleton: null or unreadable");
        SetWindowTextW(g_output, text.c_str());
        return;
    }
    append(text, L"LuaManager singleton slot: " + hex(singleton_slot, 16));
    append(text, L"LuaManager object: " + hex(manager, 16));
    append(text, L"Snapshot size: " + hex(kManagerSnapshotSize, 3));
    append(text, L"");
    append(text, L"Named fields");
    for (const auto &field : kFields) {
        std::uint64_t value{};
        const auto ok = read_memory(manager + field.offset, &value, sizeof(value));
        append(text, hex(field.offset, 3) + L"  " + field.name + L" = " + (ok ? hex(value, 16) : L"<unreadable>"));
    }
    append(text, L"");
    append(text, L"Raw qwords");
    for (std::size_t offset = 0; offset < kManagerSnapshotSize; offset += sizeof(std::uint64_t)) {
        std::uint64_t value{};
        const auto ok = read_memory(manager + offset, &value, sizeof(value));
        append(text, hex(offset, 3) + L" = " + (ok ? hex(value, 16) : L"<unreadable>"));
    }
    SetWindowTextW(g_output, text.c_str());
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_CREATE: {
        auto *create = reinterpret_cast<CREATESTRUCTW *>(lparam);
        g_output = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
            8, 40, 960, 680, window, nullptr, create->hInstance, nullptr);
        SendMessageW(g_output, EM_SETLIMITTEXT, 1u << 20, 0);
        CreateWindowW(L"BUTTON", L"Refresh", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 8, 8, 90, 26, window, reinterpret_cast<HMENU>(1), create->hInstance, nullptr);
        g_auto_refresh = CreateWindowW(L"BUTTON", L"Auto refresh", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 110, 8, 110, 26, window, reinterpret_cast<HMENU>(2), create->hInstance, nullptr);
        refresh_view();
        SetTimer(window, kRefreshTimer, 1000, nullptr);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wparam) == 1)
            refresh_view();
        else if (LOWORD(wparam) == 2)
            refresh_view();
        return 0;
    case WM_TIMER:
        if (IsDlgButtonChecked(window, 2) == BST_CHECKED)
            refresh_view();
        return 0;
    case WM_SIZE:
        if (g_output)
            MoveWindow(g_output, 8, 40, LOWORD(lparam) - 16, HIWORD(lparam) - 48, TRUE);
        return 0;
    case WM_DESTROY:
        KillTimer(window, kRefreshTimer);
        if (g_process)
            CloseHandle(g_process);
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, message, wparam, lparam);
    }
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    WNDCLASSW klass{};
    klass.hInstance = instance;
    klass.lpfnWndProc = window_proc;
    klass.lpszClassName = L"LuaManagerStructureViewer";
    klass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&klass);
    const auto window = CreateWindowW(klass.lpszClassName, L"Scrap Mechanic LuaManager Structure Viewer",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1000, 780, nullptr, nullptr, instance, nullptr);
    if (!window)
        return 1;
    ShowWindow(window, show);
    UpdateWindow(window);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}
