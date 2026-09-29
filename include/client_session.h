#pragma once

#include "kv_store.h"

namespace kv {

class ClientSession {
public:
    ClientSession(int socket_fd, KvStore& store);
    void run();

private:
    int socket_fd_;
    KvStore& store_;
};

}  // namespace kv
