#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>

namespace kv {

class KvStore {
public:
    void set(const std::string& key, const std::string& value,
             std::optional<std::int64_t> ttl_seconds = std::nullopt);
    bool get(const std::string& key, std::string& value) const;
    bool del(const std::string& key);

private:
    struct Entry {
        std::string value;
        std::optional<std::chrono::steady_clock::time_point> expires_at;
    };

    mutable std::shared_mutex mutex_;
    mutable std::unordered_map<std::string, Entry> values_;
};

}  // namespace kv
