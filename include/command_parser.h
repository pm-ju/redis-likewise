#pragma once

#include "command.h"

#include <string>

namespace kv {

class CommandParser {
public:
    static ParseResult parse(const std::string& line);
};

}  // namespace kv
