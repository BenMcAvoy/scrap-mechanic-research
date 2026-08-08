#include "directory_client.hpp"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <charconv>
#include <cctype>
#include <string_view>

#pragma comment(lib, "winhttp.lib")

namespace directory {
namespace {

struct Url {
    std::wstring host;
    std::wstring path;
    INTERNET_PORT port{};
    bool secure{};
};

std::wstring widen(std::string_view value) {
    if (value.empty())
        return {};
    const auto length = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(length, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), length);
    return result;
}

bool parse_url(std::string_view input, Url &result) {
    const auto secure = input.starts_with("https://");
    const auto prefix = secure ? 8u : input.starts_with("http://") ? 7u : 0u;
    if (!prefix)
        return false;

    const auto authority_end = input.find('/', prefix);
    const auto authority = input.substr(prefix, authority_end == std::string_view::npos ? input.size() - prefix : authority_end - prefix);
    const auto colon = authority.find(':');
    const auto host = colon == std::string_view::npos ? authority : authority.substr(0, colon);
    result.host = widen(host);
    result.port = secure ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;
    if (colon != std::string_view::npos) {
        unsigned value{};
        const auto [end, error] = std::from_chars(authority.data() + colon + 1, authority.data() + authority.size(), value);
        if (error != std::errc{} || end != authority.data() + authority.size() || value > 65535)
            return false;
        result.port = static_cast<INTERNET_PORT>(value);
    }
    result.path = widen(authority_end == std::string_view::npos ? "/v1/servers" : std::string(input.substr(authority_end)) + "/v1/servers");
    result.secure = secure;
    return !result.host.empty();
}

std::string unescape(std::string_view value) {
    std::string result;
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] != '\\' || i + 1 >= value.size()) {
            result += value[i];
            continue;
        }
        const auto escaped = value[++i];
        result += escaped == 'n' ? '\n' : escaped == 'r' ? '\r' : escaped == 't' ? '\t' : escaped;
    }
    return result;
}

std::string string_field(std::string_view object, std::string_view field) {
    const auto key = std::string("\"") + std::string(field) + "\"";
    const auto key_pos = object.find(key);
    if (key_pos == std::string_view::npos)
        return {};
    const auto colon = object.find(':', key_pos + key.size());
    const auto quote = object.find('"', colon + 1);
    if (colon == std::string_view::npos || quote == std::string_view::npos)
        return {};
    std::string value;
    bool escaped = false;
    for (auto i = quote + 1; i < object.size(); ++i) {
        if (!escaped && object[i] == '"')
            return unescape(value);
        if (!escaped && object[i] == '\\') {
            escaped = true;
            value += object[i];
        } else {
            value += object[i];
            escaped = false;
        }
    }
    return {};
}

int integer_field(std::string_view object, std::string_view field) {
    const auto key = std::string("\"") + std::string(field) + "\"";
    const auto key_pos = object.find(key);
    if (key_pos == std::string_view::npos)
        return 0;
    const auto colon = object.find(':', key_pos + key.size());
    if (colon == std::string_view::npos)
        return 0;
    const auto first = object.find_first_of("-0123456789", colon + 1);
    if (first == std::string_view::npos)
        return 0;
    int value{};
    std::from_chars(object.data() + first, object.data() + object.size(), value);
    return value;
}

std::vector<Server> parse_servers(std::string_view json) {
    std::vector<Server> result;
    for (std::size_t cursor = 0;;) {
        const auto start = json.find('{', cursor);
        if (start == std::string_view::npos)
            break;
        std::size_t depth = 0;
        bool quoted = false;
        bool escaped = false;
        std::size_t end = start;
        for (; end < json.size(); ++end) {
            const auto character = json[end];
            if (quoted) {
                if (!escaped && character == '"')
                    quoted = false;
                escaped = !escaped && character == '\\';
                if (character != '\\')
                    escaped = false;
                continue;
            }
            if (character == '"') {
                quoted = true;
                continue;
            }
            if (character == '{')
                ++depth;
            if (character == '}' && --depth == 0)
                break;
        }
        if (end >= json.size())
            break;
        const auto object = json.substr(start, end - start + 1);
        Server server;
        server.id = string_field(object, "id");
        server.name = string_field(object, "name");
        server.region = string_field(object, "region");
        server.version = string_field(object, "version");
        server.players = integer_field(object, "players");
        server.max_players = integer_field(object, "max_players");
        const auto connection = object.find("\"connection\"");
        if (connection != std::string_view::npos) {
            server.steam_id = string_field(object.substr(connection), "steam_id");
            server.secret = string_field(object.substr(connection), "secret");
        }
        if (!server.id.empty() && !server.steam_id.empty())
            result.push_back(std::move(server));
        cursor = end + 1;
    }
    return result;
}

} // namespace

Client::~Client() {
    stop();
}

bool Client::start(std::string base_url, unsigned refresh_seconds) {
    stop();
    base_url_ = std::move(base_url);
    refresh_seconds_ = std::max(1u, refresh_seconds);
    running_ = true;
    worker_ = std::thread(&Client::run, this);
    return true;
}

void Client::stop() {
    running_ = false;
    if (worker_.joinable())
        worker_.join();
}

void Client::refresh() {
    refresh_requested_ = true;
}

void Client::set_wakeup_window(HWND window) {
    wakeup_window_ = window;
}

std::vector<Event> Client::drain() {
    std::lock_guard lock(events_mutex_);
    auto result = std::move(events_);
    events_.clear();
    return result;
}

void Client::publish(Event event) {
    std::lock_guard lock(events_mutex_);
    events_.push_back(std::move(event));
    if (wakeup_window_)
        PostMessageW(wakeup_window_, WM_APP + 0x442, 0, 0);
}

bool Client::fetch(std::vector<Server> &servers, std::string &error) {
    Url url;
    if (!parse_url(base_url_, url)) {
        error = "invalid directory URL";
        return false;
    }

    auto session = WinHttpOpen(L"DedicatedHelpersClient/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, nullptr, nullptr, 0);
    auto connection = session ? WinHttpConnect(session, url.host.c_str(), url.port, 0) : nullptr;
    const auto flags = url.secure ? WINHTTP_FLAG_SECURE : 0;
    auto request = connection ? WinHttpOpenRequest(connection, L"GET", url.path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags) : nullptr;
    bool ok = false;
    if (request && WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) && WinHttpReceiveResponse(request, nullptr)) {
        std::string body;
        DWORD available{};
        while (WinHttpQueryDataAvailable(request, &available) && available) {
            const auto old_size = body.size();
            body.resize(old_size + available);
            DWORD read{};
            if (!WinHttpReadData(request, body.data() + old_size, available, &read))
                break;
            body.resize(old_size + read);
        }
        servers = parse_servers(body);
        ok = true;
    }
    if (!ok)
        error = "directory request failed";
    if (request)
        WinHttpCloseHandle(request);
    if (connection)
        WinHttpCloseHandle(connection);
    if (session)
        WinHttpCloseHandle(session);
    return ok;
}

void Client::run() {
    while (running_) {
        std::vector<Server> servers;
        std::string error;
        if (fetch(servers, error))
            publish({Event::Kind::snapshot, std::move(servers), {}});
        else
            publish({Event::Kind::status, {}, std::move(error)});

        for (unsigned second = 0; second < refresh_seconds_ && running_; ++second)
            Sleep(1000);
        refresh_requested_ = false;
    }
}

} // namespace directory
