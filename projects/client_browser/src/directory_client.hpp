#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

namespace directory {

struct Server {
    std::string id;
    std::string name;
    std::string region;
    std::string version;
    std::string steam_id;
    std::string secret;
    int players{};
    int max_players{};
};

struct Event {
    enum class Kind { snapshot, status } kind{};
    std::vector<Server> servers;
    std::string status;
};

class Client {
  public:
    Client() = default;
    ~Client();

    bool start(std::string base_url, unsigned refresh_seconds = 5);
    void set_wakeup_window(HWND window);
    void stop();
    void refresh();
    std::vector<Event> drain();

  private:
    void run();
    void publish(Event event);
    bool fetch(std::vector<Server> &servers, std::string &error);

    std::string base_url_;
    unsigned refresh_seconds_{5};
    std::atomic_bool running_{};
    std::atomic_bool refresh_requested_{};
    std::thread worker_;
    std::mutex events_mutex_;
    std::vector<Event> events_;
    HWND wakeup_window_{};
};

} // namespace directory
