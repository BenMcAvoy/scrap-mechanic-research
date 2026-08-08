#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "directory_client.hpp"
#include "memorylib/memorylib.hpp"

namespace {

using MainMenuConstructor = void *(__fastcall *)(void *);
using ConnectionStatusChanged = std::int64_t(__fastcall *)(void *, void *);
using GetGui = void *(__fastcall *)();
using CreateWidget = void *(__fastcall *)(void *, std::string *, std::string *, const struct IntCoord *, std::uint32_t, std::string *, std::string *);
using SetWidgetProperty = void(__fastcall *)(void *, std::string *, std::string *);
using ButtonClick = void(__fastcall *)(void *);
using SteamUser = std::int32_t;
using GetSteamUser = SteamUser (*)();
using FindSteamInterface = void *(*)(SteamUser, const char *);
using GetSteamId = std::uint64_t (*)(void *);

struct IntCoord {
    int left;
    int top;
    int width;
    int height;
};

struct ClientConfig {
    std::string directory_url{"http://127.0.0.1:8080"};
    unsigned refresh_seconds{5};
    unsigned max_servers{8};
};

std::mutex g_log_mutex;
mem::hook::Function<MainMenuConstructor> g_main_menu_hook;
mem::hook::Function<ConnectionStatusChanged> g_connection_status_hook;
mem::hook::Function<ButtonClick> g_button_click_hook;
GetGui g_get_gui{};
CreateWidget g_create_widget{};
SetWidgetProperty g_set_widget_property{};
HWND g_game_window{};
WNDPROC g_original_window_proc{};
void *g_browser_panel{};
void *g_browser_title{};
void *g_refresh_button{};
std::vector<void *> g_server_buttons;
std::mutex g_server_mutex;
std::vector<directory::Server> g_servers;
directory::Client g_directory;
ClientConfig g_config;
std::atomic_bool g_button_callback_installed{};
std::atomic_bool g_connection_status_hook_installed{};

void log_line(std::string_view message) {
    std::lock_guard lock(g_log_mutex);
    std::array<wchar_t, MAX_PATH> path{};
    const auto length = GetTempPathW(static_cast<DWORD>(path.size()), path.data());
    if (!length || length >= path.size())
        return;
    std::ofstream log(std::wstring(path.data(), length) + L"DedicatedHelpersClient.log", std::ios::app);
    log << message << '\n';
}

std::uint32_t callback_u32(const std::uint8_t *callback, std::size_t offset) {
    std::uint32_t value{};
    if (mem::ProcessMemory::readable(callback + offset, sizeof(value)))
        std::memcpy(&value, callback + offset, sizeof(value));
    return value;
}

std::uint64_t callback_u64(const std::uint8_t *callback, std::size_t offset) {
    std::uint64_t value{};
    if (mem::ProcessMemory::readable(callback + offset, sizeof(value)))
        std::memcpy(&value, callback + offset, sizeof(value));
    return value;
}

std::int64_t __fastcall hooked_connection_status_changed(void *play_state, void *callback) {
    const auto bytes = static_cast<const std::uint8_t *>(callback);
    log_line("client connection callback=" + std::to_string(reinterpret_cast<std::uintptr_t>(callback)) +
        " hConn=" + std::to_string(callback_u32(bytes, 0)) + " state=" + std::to_string(callback_u32(bytes, 184)) +
        " flags=" + std::to_string(callback_u32(bytes, 704)) + " remote=" +
        std::to_string(callback_u64(bytes, 8)) + " info=" + std::to_string(callback_u64(bytes, 16)));
    const auto result = g_connection_status_hook.original()(play_state, callback);
    log_line("client connection callback handled result=" + std::to_string(result));
    return result;
}

std::string config_value(std::string_view text, std::string_view key) {
    const auto position = text.find(std::string(key) + " =");
    if (position == std::string_view::npos)
        return {};
    const auto start = text.find_first_not_of(" \t\"", position + key.size() + 2);
    const auto end = text.find_first_of("\"\r\n#", start);
    return start == std::string_view::npos ? std::string() : std::string(text.substr(start, end - start));
}

void load_config() {
    std::ifstream file("DedicatedHelpersClient.toml");
    if (!file)
        return;
    const std::string text((std::istreambuf_iterator<char>(file)), {});
    if (const auto value = config_value(text, "directory_url"); !value.empty())
        g_config.directory_url = value;
    if (const auto value = config_value(text, "refresh_seconds"); !value.empty())
        g_config.refresh_seconds = std::max(1u, static_cast<unsigned>(std::stoul(value)));
    if (const auto value = config_value(text, "max_servers"); !value.empty())
        g_config.max_servers = std::clamp(static_cast<unsigned>(std::stoul(value)), 1u, 16u);
}

HWND game_window() {
    HWND result{};
    EnumWindows([](HWND window, LPARAM parameter) {
        DWORD process_id{};
        GetWindowThreadProcessId(window, &process_id);
        if (process_id == GetCurrentProcessId() && IsWindowVisible(window) && !GetWindow(window, GW_OWNER)) {
            *reinterpret_cast<HWND *>(parameter) = window;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&result));
    return result;
}

std::wstring game_executable() {
    std::array<wchar_t, MAX_PATH> path{};
    const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    return length && length < path.size() ? std::wstring(path.data(), length) : std::wstring();
}

void *create_widget(std::string_view type, std::string_view skin, const IntCoord &coord, std::string_view name) {
    if (!g_get_gui || !g_create_widget)
        return nullptr;
    std::string type_name(type);
    std::string widget_skin(skin);
    std::string layer("Back");
    std::string widget_name(name);
    return g_create_widget(g_get_gui(), &type_name, &widget_skin, &coord, 10, &layer, &widget_name);
}

void set_property(void *widget, std::string_view property, std::string_view value) {
    if (!widget || !g_set_widget_property)
        return;
    std::string property_name(property);
    std::string property_value(value);
    g_set_widget_property(widget, &property_name, &property_value);
}

IntCoord browser_coord(float left, float top, float width, float height) {
    RECT client{0, 0, 1920, 1080};
    if (g_game_window)
        GetClientRect(g_game_window, &client);
    const auto client_width = static_cast<float>(client.right - client.left);
    const auto client_height = static_cast<float>(client.bottom - client.top);
    return {static_cast<int>(client_width * left), static_cast<int>(client_height * top),
        static_cast<int>(client_width * width), static_cast<int>(client_height * height)};
}

void join_server(void *button) {
    std::size_t index{};
    {
        std::lock_guard lock(g_server_mutex);
        const auto found = std::find(g_server_buttons.begin(), g_server_buttons.end(), button);
        if (found == g_server_buttons.end())
            return;
        index = static_cast<std::size_t>(found - g_server_buttons.begin());
        if (index >= g_servers.size())
            return;
    }

    directory::Server server;
    {
        std::lock_guard lock(g_server_mutex);
        server = g_servers[index];
    }
    const auto executable = game_executable();
    if (executable.empty() || server.steam_id.empty())
        return;

    auto command_line = L"\"" + executable + L"\" -last_save -connect_steam_id " + std::wstring(server.steam_id.begin(), server.steam_id.end());
    command_line += L" -friend_steam_id " + std::wstring(server.steam_id.begin(), server.steam_id.end());
    if (!server.secret.empty())
        command_line += L" -secret " + std::wstring(server.secret.begin(), server.secret.end());
    std::vector<wchar_t> mutable_command(command_line.begin(), command_line.end());
    mutable_command.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), mutable_command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process))
        return;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (g_game_window)
        PostMessageW(g_game_window, WM_CLOSE, 0, 0);
    log_line("launching directory server " + server.id + " with -last_save through native Steam connection flow");
}

void __fastcall hooked_button_click(void *button) {
    if (g_button_click_hook.original())
        g_button_click_hook.original()(button);
    if (button == g_refresh_button)
        g_directory.refresh();
    else
        join_server(button);
}

bool install_button_callback() {
    bool expected = false;
    if (!g_button_callback_installed.compare_exchange_strong(expected, true))
        return true;
    if (g_server_buttons.empty() || !g_server_buttons.front()) {
        g_button_callback_installed = false;
        return false;
    }
    auto vtable = *reinterpret_cast<const std::uintptr_t **>(g_server_buttons.front());
    if (!vtable) {
        g_button_callback_installed = false;
        return false;
    }
    constexpr std::size_t on_mouse_button_click_slot = 9;
    auto target = reinterpret_cast<ButtonClick>(vtable[on_mouse_button_click_slot]);
    if (!target || !g_button_click_hook.install(target, hooked_button_click)) {
        g_button_callback_installed = false;
        log_line("MyGUI button click callback installation failed");
        return false;
    }
    log_line("MyGUI button click callback installed");
    return true;
}

void update_browser() {
    for (const auto &event : g_directory.drain()) {
        if (event.kind == directory::Event::Kind::status) {
            set_property(g_browser_title, "Caption", "DedicatedHelpers Servers - " + event.status);
            continue;
        }
        std::lock_guard lock(g_server_mutex);
        g_servers = event.servers;
        if (g_servers.size() > g_server_buttons.size())
            g_servers.resize(g_server_buttons.size());
        for (std::size_t index = 0; index < g_server_buttons.size(); ++index) {
            if (index < g_servers.size()) {
                const auto &server = g_servers[index];
                set_property(g_server_buttons[index], "Caption", server.name + "  [" + std::to_string(server.players) + "/" + std::to_string(server.max_players) + "]");
                set_property(g_server_buttons[index], "Enabled", "true");
            } else {
                set_property(g_server_buttons[index], "Caption", "No server");
                set_property(g_server_buttons[index], "Enabled", "false");
            }
        }
        set_property(g_browser_title, "Caption", "DedicatedHelpers Servers (" + std::to_string(g_servers.size()) + ")");
    }
}

LRESULT CALLBACK hooked_window_proc(HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
    if (message == WM_APP + 0x442) {
        update_browser();
        return 0;
    }
    return g_original_window_proc ? CallWindowProcW(g_original_window_proc, window, message, w_param, l_param)
        : DefWindowProcW(window, message, w_param, l_param);
}

void install_message_bridge() {
    g_game_window = game_window();
    if (!g_game_window || g_original_window_proc)
        return;
    g_original_window_proc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(g_game_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(hooked_window_proc)));
    g_directory.set_wakeup_window(g_game_window);
}

bool connect_menu_controls(mem::Scan &scan) {
    auto create_widget_pattern = scan.pattern(
        "48 83 EC 48 48 8B 84 24 ?? ?? ?? ?? 48 89 44 24 ?? 48 8B 44 24 ?? 48 89 44 24 ?? 8B 44 24 ??",
        "MyGUI widget creation");
    auto set_property_pattern = scan.pattern(
        "48 89 5C 24 ?? 48 89 74 24 ?? 55 57 41 54 41 56 41 57 48 8D 6C 24 ?? 48 81 EC 20 01 00 00 4D 8B F8",
        "MyGUI widget property dispatch");
    if (!create_widget_pattern || !set_property_pattern)
        return false;

    const auto constructor_range = scan.function_range(reinterpret_cast<const std::uint8_t *>(g_main_menu_hook.target()), "main menu constructor range");
    const std::uint8_t *get_gui{};
    if (constructor_range) {
        const auto calls = mem::relative_calls(constructor_range.get());
        for (const auto &call : calls) {
            if (!mem::ProcessMemory::readable(call.instruction + 5, 3) || call.instruction[5] != 0x48 || call.instruction[6] != 0x8B || call.instruction[7] != 0xD8)
                continue;
            const auto global = mem::resolve_rip_target(call.target);
            const auto gui = mem::read<void *>(global);
            if (gui && gui.get() && mem::ProcessMemory::readable(gui.get(), 0x50))
                get_gui = call.target;
        }
    }
    if (!get_gui)
        return false;

    g_get_gui = reinterpret_cast<GetGui>(const_cast<std::uint8_t *>(get_gui));
    g_create_widget = reinterpret_cast<CreateWidget>(const_cast<std::uint8_t *>(create_widget_pattern.get()));
    g_set_widget_property = reinterpret_cast<SetWidgetProperty>(const_cast<std::uint8_t *>(set_property_pattern.get()));

    g_browser_panel = create_widget("Widget", "PanelEmpty", browser_coord(0.68f, 0.20f, 0.30f, 0.72f), "DedicatedHelpersPanel");
    g_browser_title = create_widget("TextBox", "TextBox", browser_coord(0.695f, 0.23f, 0.27f, 0.055f), "DedicatedHelpersTitle");
    set_property(g_browser_title, "Caption", "DedicatedHelpers Servers - loading");
    set_property(g_browser_title, "FontName", "SM_ButtonLarge");
    set_property(g_browser_title, "TextAlign", "Center VCenter");
    g_refresh_button = create_widget("Button", "MenuButton", browser_coord(0.695f, 0.30f, 0.27f, 0.055f), "DedicatedHelpersRefresh");
    set_property(g_refresh_button, "Caption", "Refresh server list");

    g_server_buttons.resize(g_config.max_servers);
    for (std::size_t index = 0; index < g_server_buttons.size(); ++index) {
        const auto top = 0.38f + static_cast<float>(index) * 0.065f;
        g_server_buttons[index] = create_widget("Button", "MenuButton", browser_coord(0.695f, top, 0.27f, 0.055f), "DedicatedHelpersServer" + std::to_string(index));
        set_property(g_server_buttons[index], "Caption", "No server");
        set_property(g_server_buttons[index], "Enabled", "false");
    }
    return install_button_callback();
}

bool install_connection_status_hook(mem::Scan &scan) {
    log_line("client connection status scan begin");
    auto anchor = scan.string_xref("Connection Status Changed", "client connection status anchor");
    if (!anchor) {
        log_line("client connection status anchor failed: " + anchor.error.message);
        return false;
    }
    auto function = scan.containing_function(anchor.get(), "client connection status function");
    if (!function) {
        log_line("client connection status function failed: " + function.error.message);
        return false;
    }
    auto installed = g_connection_status_hook.install(reinterpret_cast<ConnectionStatusChanged>(const_cast<std::uint8_t *>(function.get())),
        hooked_connection_status_changed);
    g_connection_status_hook_installed.store(static_cast<bool>(installed), std::memory_order_release);
    log_line("client connection status hook=" + std::string(installed ? "installed" : installed.error.message));
    return static_cast<bool>(installed);
}

void * __fastcall hooked_main_menu_constructor(void *self) {
    const auto result = g_main_menu_hook.original()(self);
    load_config();
    install_message_bridge();
    auto scan_result = mem::Scan::open(L"ScrapMechanic.exe");
    if (scan_result && connect_menu_controls(scan_result.get())) {
        g_directory.start(g_config.directory_url, g_config.refresh_seconds);
        g_directory.refresh();
        log_line("directory-backed browser initialized at " + g_config.directory_url);
    } else {
        log_line("directory-backed browser initialization failed");
    }
    return result;
}

DWORD WINAPI bootstrap(void *) {
    log_line("client bootstrap started build 20260808");
    for (unsigned attempt = 0; attempt < 120; ++attempt) {
        auto scan_result = mem::Scan::open(L"ScrapMechanic.exe");
        if (scan_result) {
            auto &scan = scan_result.get();
            auto constructor = scan.pattern(
                "48 8B C4 48 89 58 10 48 89 70 18 48 89 78 20 55 41 54 41 55 41 56 41 57 48 8D A8 08 FF FF FF 48 81 EC D0 01 00 00",
                "main menu root constructor");
            if (!constructor) {
                auto layout = scan.string_xref("$GAME_DATA/Gui/Layouts/MainMenu/MainMenu.layout", "main menu layout");
                if (layout)
                    constructor = scan.containing_function(layout.get(), "main menu root constructor");
            }
            if (constructor && mem::hook::initialize() && g_main_menu_hook.install(reinterpret_cast<MainMenuConstructor>(const_cast<std::uint8_t *>(constructor.get())), hooked_main_menu_constructor)) {
                log_line("client diagnostics build 20260806-connection-status");
                install_connection_status_hook(scan);
                return 0;
            }
        }
        Sleep(1000);
    }
    log_line("main menu constructor scan timed out");
    return 0;
}

} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        if (auto thread = CreateThread(nullptr, 0, bootstrap, nullptr, 0, nullptr))
            CloseHandle(thread);
    }
    return TRUE;
}
