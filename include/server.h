#pragma once

#include "kv_store.h"
#include "thread_pool.h"

#include <cstdint>
#include <cstddef>
#include <string>

namespace kv {

bool shutdown_requested();

class Server {
public:
    Server(std::string address, std::uint16_t port, std::size_t worker_count);
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    bool run();
    void request_shutdown();

private:
    bool open_listener();
    void close_listener();

    std::string address_;
    std::uint16_t port_;
    int listen_fd_ = -1;
    KvStore store_;
    ThreadPool workers_;
};

}  // namespace kv
