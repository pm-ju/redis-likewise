#include "server.h"

#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
std::uint16_t parse_port(const char* text) {
    try {
        const unsigned long value = std::stoul(text);
        if (value == 0 || value > std::numeric_limits<std::uint16_t>::max()) throw std::out_of_range("port");
        return static_cast<std::uint16_t>(value);
    } catch (...) {
        throw std::invalid_argument("port must be an integer from 1 to 65535");
    }
}

std::size_t parse_workers(const char* text) {
    try {
        const unsigned long value = std::stoul(text);
        if (value == 0 || value > 256) throw std::out_of_range("workers");
        return static_cast<std::size_t>(value);
    } catch (...) {
        throw std::invalid_argument("worker count must be an integer from 1 to 256");
    }
}
}

int main(int argc, char* argv[]) {
    if (argc > 3) { std::cerr << "Usage: kv_server [port] [workers]\n"; return 2; }
    std::uint16_t port = 6380;
    std::size_t workers = std::thread::hardware_concurrency();
    if (workers == 0) workers = 4;
    try {
        if (argc >= 2) port = parse_port(argv[1]);
        if (argc == 3) workers = parse_workers(argv[2]);
    }
    catch (const std::exception& error) { std::cerr << "[ERROR] " << error.what() << '\n'; return 2; }
    try {
        return kv::Server("127.0.0.1", port, workers).run() ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "[ERROR] " << error.what() << '\n';
        return 1;
    }
}
