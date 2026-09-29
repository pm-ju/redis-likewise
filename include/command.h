#pragma once

#include <string>
#include <cstdint>
#include <optional>

namespace kv {

enum class CommandType { Ping, Set, Get, Del, Help, Quit };

struct Command {
    CommandType type;
    std::string key;
    std::string value;
    std::optional<std::int64_t> ttl_seconds;
};

struct ParseResult {
    bool ok;
    Command command{};
    std::string error;
};

}  // namespace kv
