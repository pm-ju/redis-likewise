#include "client_session.h"

#include "command_parser.h"
#include "server.h"

#include <cerrno>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace kv {
namespace {
constexpr std::size_t kMaxLineLength = 4096;
constexpr std::size_t kMaxKeyLength = 256;
constexpr std::size_t kMaxValueLength = 2048;

bool send_all(int socket_fd, const std::string& response) {
    std::size_t sent = 0;
    while (sent < response.size()) {
        const ssize_t count = send(socket_fd, response.data() + sent, response.size() - sent, MSG_NOSIGNAL);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        sent += static_cast<std::size_t>(count);
    }
    return true;
}

std::string execute(const Command& command, KvStore& store, bool& close) {
    switch (command.type) {
    case CommandType::Ping: return "PONG\n";
    case CommandType::Help: return "PING\nSET <key> <value>\nGET <key>\nDEL <key>\nHELP\nQUIT\n";
    case CommandType::Set:
        if (command.key.size() > kMaxKeyLength || command.value.size() > kMaxValueLength)
            return "(error) key or value too long\n";
        store.set(command.key, command.value, command.ttl_seconds);
        return "OK\n";
    case CommandType::Get: {
        if (command.key.size() > kMaxKeyLength) return "(error) key too long\n";
        std::string value;
        return store.get(command.key, value) ? value + "\n" : "(nil)\n";
    }
    case CommandType::Del:
        if (command.key.size() > kMaxKeyLength) return "(error) key too long\n";
        return store.del(command.key) ? "(integer) 1\n" : "(integer) 0\n";
    case CommandType::Quit: close = true; return "BYE\n";
    }
    return "(error) unknown command\n";
}

}  // namespace

ClientSession::ClientSession(int socket_fd, KvStore& store) : socket_fd_(socket_fd), store_(store) {}

void ClientSession::run() {
    timeval timeout{0, 250000};
    setsockopt(socket_fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    std::string buffer;
    char chunk[1024];
    bool close = false;
    while (!close) {
        const ssize_t received = recv(socket_fd_, chunk, sizeof(chunk), 0);
        if (received == 0) break;
        if (received < 0) {
            if (errno == EINTR) {
                if (shutdown_requested()) break;
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (shutdown_requested()) break;
                continue;
            }
            std::cerr << "[ERROR] Failed to receive client data: " << std::strerror(errno) << '\n';
            break;
        }
        buffer.append(chunk, static_cast<std::size_t>(received));
        while (!close) {
            const auto newline = buffer.find('\n');
            if (newline == std::string::npos) {
                if (buffer.size() > kMaxLineLength) {
                    if (!send_all(socket_fd_, "(error) command line too long\n")) return;
                    buffer.clear();
                }
                break;
            }
            std::string line = buffer.substr(0, newline);
            buffer.erase(0, newline + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.size() > kMaxLineLength) {
                if (!send_all(socket_fd_, "(error) command line too long\n")) return;
                continue;
            }
            const ParseResult parsed = CommandParser::parse(line);
            const std::string response = parsed.ok ? execute(parsed.command, store_, close) : parsed.error + "\n";
            if (!send_all(socket_fd_, response)) return;
        }
    }
}

}  // namespace kv
