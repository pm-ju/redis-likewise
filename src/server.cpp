#include "server.h"

#include "client_session.h"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <utility>

namespace kv {
namespace {
volatile std::sig_atomic_t shutdown_flag = 0;

void handle_sigint(int) { shutdown_flag = 1; }

void log_error(const char* action) { std::cerr << "[ERROR] " << action << ": " << std::strerror(errno) << '\n'; }
}  // namespace

bool shutdown_requested() { return shutdown_flag != 0; }

Server::Server(std::string address, std::uint16_t port, std::size_t worker_count)
    : address_(std::move(address)), port_(port), workers_(worker_count, 64) {}
Server::~Server() { close_listener(); }

bool Server::open_listener() {
    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) { log_error("Failed to create socket"); return false; }
    int reuse = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in endpoint{};
    endpoint.sin_family = AF_INET;
    endpoint.sin_port = htons(port_);
    if (inet_pton(AF_INET, address_.c_str(), &endpoint.sin_addr) != 1) {
        std::cerr << "[ERROR] Invalid IPv4 address: " << address_ << '\n'; return false;
    }
    if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&endpoint), sizeof(endpoint)) < 0) { log_error("Failed to bind socket"); return false; }
    if (listen(listen_fd_, 16) < 0) { log_error("Failed to listen"); return false; }
    std::cout << "[INFO] Server listening on " << address_ << ':' << port_ << '\n';
    return true;
}

void Server::close_listener() { if (listen_fd_ >= 0) { close(listen_fd_); listen_fd_ = -1; } }
void Server::request_shutdown() { shutdown_flag = 1; }

bool Server::run() {
    shutdown_flag = 0;
    std::signal(SIGINT, handle_sigint);
    if (!open_listener()) return false;
    while (!shutdown_requested()) {
        fd_set readable;
        FD_ZERO(&readable); FD_SET(listen_fd_, &readable);
        timeval timeout{1, 0};
        const int ready = select(listen_fd_ + 1, &readable, nullptr, nullptr, &timeout);
        if (ready < 0) { if (errno == EINTR) continue; log_error("Failed while waiting for clients"); break; }
        if (ready == 0) continue;
        const int client_fd = accept(listen_fd_, nullptr, nullptr);
        if (client_fd < 0) { if (errno == EINTR) continue; log_error("Failed to accept client"); continue; }
        const bool accepted = workers_.submit([this, client_fd] {
            struct SocketGuard {
                int fd;
                ~SocketGuard() { if (fd >= 0) close(fd); }
            } guard{client_fd};
            std::cout << "[INFO] Client connected\n";
            ClientSession(guard.fd, store_).run();
            std::cout << "[INFO] Client disconnected\n";
        });
        if (!accepted) {
            const char response[] = "(error) server busy\n";
            send(client_fd, response, sizeof(response) - 1, MSG_NOSIGNAL);
            close(client_fd);
        }
    }
    close_listener();
    workers_.shutdown();
    std::cout << "[INFO] Server stopped\n";
    return true;
}

}  // namespace kv
