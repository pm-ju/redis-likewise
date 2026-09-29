#include "command_parser.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <sstream>
#include <vector>

namespace kv {
namespace {

std::string upper(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char character) { return static_cast<char>(std::toupper(character)); });
    return value;
}

std::vector<std::string> split(const std::string& line) {
    std::istringstream input(line);
    std::vector<std::string> tokens;
    std::string token;
    while (input >> token) {
        tokens.push_back(token);
    }
    return tokens;
}

ParseResult wrong_count(const std::string& name) {
    return {false, {}, "(error) wrong number of arguments for " + name};
}

bool looks_numeric(const std::string& value) {
    if (value.empty()) return false;
    const std::size_t start = value[0] == '-' || value[0] == '+' ? 1 : 0;
    if (start == value.size()) return false;
    return std::all_of(value.begin() + static_cast<std::ptrdiff_t>(start), value.end(),
                       [](unsigned char character) { return std::isdigit(character) != 0; });
}

bool parse_ttl(const std::string& value, std::int64_t& ttl) {
    if (!looks_numeric(value)) return false;
    try {
        std::size_t consumed = 0;
        const long long parsed = std::stoll(value, &consumed);
        constexpr long long kMaxTtlSeconds = 10LL * 365LL * 24LL * 60LL * 60LL;
        if (consumed != value.size() || parsed < 0 || parsed > kMaxTtlSeconds) return false;
        ttl = static_cast<std::int64_t>(parsed);
        return true;
    } catch (...) {
        return false;
    }
}

}  // namespace

ParseResult CommandParser::parse(const std::string& line) {
    const auto tokens = split(line);
    if (tokens.empty()) {
        return {false, {}, "(error) empty command"};
    }

    const std::string name = upper(tokens.front());
    if (name == "PING") {
        return tokens.size() == 1 ? ParseResult{true, {CommandType::Ping, {}, {}, {}}, {}} : wrong_count("PING");
    }
    if (name == "HELP") {
        return tokens.size() == 1 ? ParseResult{true, {CommandType::Help, {}, {}, {}}, {}} : wrong_count("HELP");
    }
    if (name == "QUIT") {
        return tokens.size() == 1 ? ParseResult{true, {CommandType::Quit, {}, {}, {}}, {}} : wrong_count("QUIT");
    }
    if (name == "GET" || name == "DEL") {
        if (tokens.size() != 2) return wrong_count(name);
        return {true, {name == "GET" ? CommandType::Get : CommandType::Del, tokens[1], {}, {}}, {}};
    }
    if (name == "SET") {
        if (tokens.size() < 3) return wrong_count("SET");
        std::size_t value_end = tokens.size();
        std::optional<std::int64_t> ttl;
        if (tokens.size() >= 4 && looks_numeric(tokens.back())) {
            std::int64_t parsed_ttl = 0;
            if (!parse_ttl(tokens.back(), parsed_ttl)) return {false, {}, "(error) invalid TTL"};
            ttl = parsed_ttl;
            value_end--;
        }
        std::string value = tokens[2];
        for (std::size_t index = 3; index < value_end; ++index) value += " " + tokens[index];
        return {true, {CommandType::Set, tokens[1], value, ttl}, {}};
    }
    return {false, {}, "(error) unknown command"};
}

}  // namespace kv
