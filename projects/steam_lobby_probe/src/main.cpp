#include <windows.h>

#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using SteamApiCall = std::uint64_t;
using SteamUser = std::int32_t;
using LobbyId = std::uint64_t;

constexpr int lobby_match_list_callback = 510;

using SteamApiInit = bool (*)();
using SteamApiShutdown = void (*)();
using SteamApiRunCallbacks = void (*)();
using SteamApiGetHSteamUser = SteamUser (*)();
using SteamInternalFindOrCreateUserInterface = void *(*)(SteamUser, const char *);

using RegisterCallback = void (*)(void *, int);
using RequestLobbyList = SteamApiCall (*)(void *);
using AddLobbyStringFilter = void (*)(void *, const char *, const char *, int);
using GetLobbyByIndex = LobbyId (*)(void *, int);
using GetLobbyData = const char *(*)(void *, LobbyId, const char *);
using GetLobbyOwner = std::uint64_t (*)(void *, LobbyId);
using CreateLobby = SteamApiCall (*)(void *, int, int);
using IsCallCompleted = bool (*)(void *, SteamApiCall, bool *);
using GetCallResult = bool (*)(void *, SteamApiCall, void *, int, int, bool *);
using GetCallFailureReason = int (*)(void *, SteamApiCall);
using SetLobbyData = bool (*)(void *, LobbyId, const char *, const char *);

struct LobbyMatchList {
    std::uint32_t count;
};

class CallbackBase {
  public:

    CallbackBase() : flags_(0), callback_id_(0) {}
    virtual void run(void *) = 0;
    virtual void run_call_result(void *, bool, SteamApiCall) = 0;
    virtual int callback_size_bytes() = 0;

    int callback_id() const {
        return callback_id_;
    }

  protected:

    std::uint8_t flags_;
    int callback_id_;
};

struct SteamApi {
    HMODULE module{};
    SteamApiInit init{};
    SteamApiShutdown shutdown{};
    SteamApiRunCallbacks run_callbacks{};
    SteamApiGetHSteamUser get_hsteam_user{};
    SteamInternalFindOrCreateUserInterface find_or_create_user_interface{};
    RegisterCallback register_callback{};
};

template <typename Function>
Function find_export(HMODULE module, const char *name) {
    return reinterpret_cast<Function>(GetProcAddress(module, name));
}

bool load_api(SteamApi &api) {
    api.module = LoadLibraryW(L"steam_api64.dll");
    if (!api.module) {
        std::printf("LoadLibraryW(steam_api64.dll) failed: %lu\n", GetLastError());
        return false;
    }

    api.init = find_export<SteamApiInit>(api.module, "SteamAPI_Init");
    api.shutdown = find_export<SteamApiShutdown>(api.module, "SteamAPI_Shutdown");
    api.run_callbacks = find_export<SteamApiRunCallbacks>(api.module, "SteamAPI_RunCallbacks");
    api.get_hsteam_user = find_export<SteamApiGetHSteamUser>(api.module, "SteamAPI_GetHSteamUser");
    api.find_or_create_user_interface = find_export<SteamInternalFindOrCreateUserInterface>(
        api.module, "SteamInternal_FindOrCreateUserInterface");
    api.register_callback = find_export<RegisterCallback>(api.module, "SteamAPI_RegisterCallback");
    return api.init && api.shutdown && api.run_callbacks && api.get_hsteam_user && api.find_or_create_user_interface && api.register_callback;
}

}

struct Lobby {
    LobbyId id{};
    std::uint64_t owner{};
    std::string name;
    std::string version;
};

class LobbyMatchListCallback final : public CallbackBase {
  public:

    explicit LobbyMatchListCallback(void *matchmaking) : matchmaking_(matchmaking) {
        callback_id_ = lobby_match_list_callback;
    }

    void run(void *data) override {
        if (!data)
            return;

        const auto count = static_cast<LobbyMatchList *>(data)->count;
        std::vector<Lobby> lobbies;
        for (std::uint32_t index = 0; index < count; ++index) {
            const auto id = get_lobby_by_index_(matchmaking_, static_cast<int>(index));
            if (!id)
                continue;

            const auto *server = get_lobby_data_(matchmaking_, id, "dh_server");
            if (!server || std::string_view(server) != "1")
                continue;

            const auto *name = get_lobby_data_(matchmaking_, id, "name");
            const auto *version = get_lobby_data_(matchmaking_, id, "dh_version");
            lobbies.push_back({id, get_lobby_owner_(matchmaking_, id), name ? name : "(unnamed)", version ? version : ""});
        }

        std::printf("LobbyMatchList: advertised=%u dedicated=%zu\n", count, lobbies.size());
        for (const auto &lobby : lobbies)
            std::printf("  name=%s owner=%llu lobby=%llu version=%s\n", lobby.name.c_str(),
                static_cast<unsigned long long>(lobby.owner), static_cast<unsigned long long>(lobby.id), lobby.version.c_str());
        received_ = true;
    }

    void run_call_result(void *, bool, SteamApiCall) override {}

    int callback_size_bytes() override {
        return sizeof(LobbyMatchList);
    }

    bool received() const {
        return received_;
    }

    static GetLobbyByIndex get_lobby_by_index_;
    static GetLobbyData get_lobby_data_;
    static GetLobbyOwner get_lobby_owner_;

  private:

    void *matchmaking_{};
    bool received_{};
};

GetLobbyByIndex LobbyMatchListCallback::get_lobby_by_index_{};
GetLobbyData LobbyMatchListCallback::get_lobby_data_{};
GetLobbyOwner LobbyMatchListCallback::get_lobby_owner_{};

int main() {
    FILE *log_file{};
    freopen_s(&log_file, "steam_lobby_probe.log", "w", stdout);
    setvbuf(stdout, nullptr, _IONBF, 0);

    SteamApi api;
    if (!load_api(api)) {
        std::printf("steam_api64.dll is missing required exports\n");
        return 1;
    }

    if (!api.init()) {
        std::printf("SteamAPI_Init failed. Is Steam running, and does steam_appid.txt match the game?\n");
        return 1;
    }

    const auto user = api.get_hsteam_user();
    const char *matchmaking_version = __argc > 2 ? __argv[2] : "SteamMatchMaking009";
    auto *matchmaking = api.find_or_create_user_interface(user, matchmaking_version);
    if (!matchmaking) {
        std::printf("%s is unavailable\n", matchmaking_version);
        api.shutdown();
        return 1;
    }
    std::printf("Using %s at %p\n", matchmaking_version, matchmaking);

    const auto request_lobby_list = find_export<RequestLobbyList>(api.module, "SteamAPI_ISteamMatchmaking_RequestLobbyList");
    const auto add_string_filter = find_export<AddLobbyStringFilter>(api.module, "SteamAPI_ISteamMatchmaking_AddRequestLobbyListStringFilter");
    LobbyMatchListCallback::get_lobby_by_index_ = find_export<GetLobbyByIndex>(api.module, "SteamAPI_ISteamMatchmaking_GetLobbyByIndex");
    LobbyMatchListCallback::get_lobby_data_ = find_export<GetLobbyData>(api.module, "SteamAPI_ISteamMatchmaking_GetLobbyData");
    LobbyMatchListCallback::get_lobby_owner_ = find_export<GetLobbyOwner>(api.module, "SteamAPI_ISteamMatchmaking_GetLobbyOwner");
    if (!request_lobby_list || !add_string_filter || !LobbyMatchListCallback::get_lobby_by_index_ ||
        !LobbyMatchListCallback::get_lobby_data_ || !LobbyMatchListCallback::get_lobby_owner_) {
        std::printf("Steam matchmaking interface methods are unavailable\n");
        api.shutdown();
        return 1;
    }

    if (__argc > 1 && std::strcmp(__argv[1], "--create") == 0) {
        const auto create_lobby = find_export<CreateLobby>(api.module, "SteamAPI_ISteamMatchmaking_CreateLobby");
        const auto is_call_completed = find_export<IsCallCompleted>(api.module, "SteamAPI_ISteamUtils_IsAPICallCompleted");
        const auto get_call_result = find_export<GetCallResult>(api.module, "SteamAPI_ISteamUtils_GetAPICallResult");
        const auto get_call_failure_reason = find_export<GetCallFailureReason>(api.module, "SteamAPI_ISteamUtils_GetAPICallFailureReason");
        const auto set_lobby_data = find_export<SetLobbyData>(api.module, "SteamAPI_ISteamMatchmaking_SetLobbyData");
        auto *utils = api.find_or_create_user_interface(user, "SteamUtils010");
        if (!create_lobby || !is_call_completed || !get_call_result || !set_lobby_data || !utils) {
            std::printf("Steam lobby creation methods are unavailable\n");
            api.shutdown();
            return 1;
        }

        const auto lobby_type = __argc > 3 ? std::atoi(__argv[3]) : 2;
        const auto call = create_lobby(matchmaking, lobby_type, 4);
        std::printf("CreateLobby request=%llu\n", static_cast<unsigned long long>(call));
        for (int attempt = 0; attempt < 200; ++attempt) {
            api.run_callbacks();
            bool completed{};
            const auto query = is_call_completed(utils, call, &completed);
            if (query && completed) {
                std::uint8_t result[16]{};
                bool io_failure{};
                const auto read = get_call_result(utils, call, result, sizeof(result), 513, &io_failure);
                std::printf("CreateLobby completed read=%d io_failure=%d result=%d lobby=%llu\n", static_cast<int>(read),
                    static_cast<int>(io_failure), *reinterpret_cast<std::int32_t *>(result),
                    static_cast<unsigned long long>(*reinterpret_cast<std::uint64_t *>(result + 8)));
                if (!read && get_call_failure_reason)
                    std::printf("CreateLobby failure reason=%d\n", get_call_failure_reason(utils, call));
                if (read)
                    set_lobby_data(matchmaking, *reinterpret_cast<std::uint64_t *>(result + 8), "dh_server", "1");
                api.shutdown();
                return read ? 0 : 1;
            }
            if (attempt % 20 == 0)
                std::printf("CreateLobby pending query=%d completed=%d\n", static_cast<int>(query), static_cast<int>(completed));
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        std::printf("CreateLobby did not complete within 10 seconds\n");
        api.shutdown();
        return 1;
    }

    LobbyMatchListCallback lobby_callback(matchmaking);
    api.register_callback(&lobby_callback, lobby_match_list_callback);

    const bool once = __argc > 1 && std::strcmp(__argv[1], "--once") == 0;
    add_string_filter(matchmaking, "dh_server", "1", 0);
    const auto request = request_lobby_list(matchmaking);
    std::printf("Steam initialized. Lobby list request=%llu\n", static_cast<unsigned long long>(request));

    for (int attempt = 0; attempt < 100; ++attempt) {
        api.run_callbacks();
        if (lobby_callback.received())
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    if (!lobby_callback.received())
        std::printf("Lobby list callback did not arrive within 5 seconds\n");

    if (once) {
        api.shutdown();
        return 0;
    }

    std::printf("Browser is running. Press Ctrl+C to exit. Refreshing every 10 seconds.\n");
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(10));
        add_string_filter(matchmaking, "dh_server", "1", 0);
        request_lobby_list(matchmaking);
        for (int attempt = 0; attempt < 100; ++attempt) {
            api.run_callbacks();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

}
