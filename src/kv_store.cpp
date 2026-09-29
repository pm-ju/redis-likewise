#include "kv_store.h"

#include <mutex>

namespace kv {

void KvStore::set(const std::string& key, const std::string& value,
                  std::optional<std::int64_t> ttl_seconds) {
    std::optional<std::chrono::steady_clock::time_point> expires_at;
    if (ttl_seconds.has_value()) {
        expires_at = std::chrono::steady_clock::now() + std::chrono::seconds(*ttl_seconds);
    }
    std::unique_lock<std::shared_mutex> lock(mutex_);
    values_[key] = Entry{value, expires_at};
}

bool KvStore::get(const std::string& key, std::string& value) const {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    const auto found = values_.find(key);
    if (found == values_.end()) {
        return false;
    }
    if (found->second.expires_at.has_value() &&
        std::chrono::steady_clock::now() >= *found->second.expires_at) {
        values_.erase(found);
        return false;
    }
    value = found->second.value;
    return true;
}

bool KvStore::del(const std::string& key) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    const auto found = values_.find(key);
    if (found == values_.end()) return false;
    if (found->second.expires_at.has_value() &&
        std::chrono::steady_clock::now() >= *found->second.expires_at) {
        values_.erase(found);
        return false;
    }
    values_.erase(found);
    return true;
}

}  // namespace kv
