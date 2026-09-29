#include "command_parser.h"
#include "kv_store.h"
#include "thread_pool.h"

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <netinet/in.h>
#include <string>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
void check(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}

void unit_tests() {
    kv::KvStore store;
    std::string value;
    store.set("name", "Rupdip");
    check(store.get("name", value) && value == "Rupdip", "persistent set/get");
    store.set("name", "Rahul");
    check(store.get("name", value) && value == "Rahul", "overwrite");
    check(!store.get("missing", value), "missing key");
    check(store.del("name") && !store.del("name"), "delete behavior");
    std::atomic<bool> storage_success{true};
    std::vector<std::thread> storage_workers;
    for (int worker = 0; worker < 8; ++worker) {
        storage_workers.emplace_back([worker, &store, &storage_success] {
            for (int operation = 0; operation < 1000; ++operation) {
                const std::string key = "worker_" + std::to_string(worker) + "_" + std::to_string(operation);
                store.set(key, "value");
                std::string result;
                if (!store.get(key, result) || result != "value") storage_success = false;
            }
        });
    }
    for (auto& worker : storage_workers) worker.join();
    check(storage_success.load(), "concurrent storage operations");
    store.set("immediate", "value", 0);
    check(!store.get("immediate", value), "zero TTL expires immediately");
    store.set("temporary", "value", 1);
    check(store.get("temporary", value), "TTL value initially exists");
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    check(!store.get("temporary", value), "TTL value expires lazily");

    auto parsed = kv::CommandParser::parse("sEt message hello world");
    check(parsed.ok && parsed.command.value == "hello world" && !parsed.command.ttl_seconds,
          "case and values containing spaces");
    parsed = kv::CommandParser::parse("SET session Rupdip 10");
    check(parsed.ok && parsed.command.value == "Rupdip" && parsed.command.ttl_seconds == 10,
          "TTL parsing");
    check(!kv::CommandParser::parse("SET key value -1").ok, "negative TTL rejected");
    check(!kv::CommandParser::parse("SET key value 999999999999999999999999").ok,
          "overflowing TTL rejected");
    check(!kv::CommandParser::parse("GET").ok, "argument validation");
    check(kv::CommandParser::parse("GET key\r").ok, "CRLF parsing");
    check(kv::CommandParser::parse("UNKNOWN").error == "(error) unknown command", "unknown command");

    kv::ThreadPool pool(4, 32);
    std::atomic<int> completed{0};
    for (int index = 0; index < 32; ++index) {
        check(pool.submit([&completed] { completed.fetch_add(1, std::memory_order_relaxed); }), "pool submit");
    }
    pool.shutdown();
    check(completed == 32, "pool drains queued tasks");
}

int connect_to(int port) {
    sockaddr_in endpoint{};
    endpoint.sin_family = AF_INET;
    endpoint.sin_port = htons(static_cast<std::uint16_t>(port));
    inet_pton(AF_INET, "127.0.0.1", &endpoint.sin_addr);
    for (int attempt = 0; attempt < 100; ++attempt) {
        const int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd >= 0 && connect(fd, reinterpret_cast<sockaddr*>(&endpoint), sizeof(endpoint)) == 0) return fd;
        if (fd >= 0) close(fd);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return -1;
}

void send_all(int fd, const std::string& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t count = send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        check(count > 0, "send data");
        sent += static_cast<std::size_t>(count);
    }
}

std::string read_at_least(int fd, std::size_t length) {
    std::string result;
    char buffer[1024];
    while (result.size() < length) {
        const ssize_t count = recv(fd, buffer, sizeof(buffer), 0);
        check(count > 0, "receive response");
        result.append(buffer, static_cast<std::size_t>(count));
    }
    return result;
}

std::string read_line(int fd) {
    std::string result;
    char character = '\0';
    while (true) {
        check(recv(fd, &character, 1, 0) == 1, "receive line");
        result += character;
        if (character == '\n') return result;
    }
}

void integration_tests(const char* server_path) {
    constexpr int port = 6391;
    const pid_t child = fork();
    check(child >= 0, "fork server");
    if (child == 0) {
        execl(server_path, server_path, "6391", "4", static_cast<char*>(nullptr));
        _exit(127);
    }

    const int fd = connect_to(port);
    check(fd >= 0, "connect to server");
    const std::string first = "PING\r";
    send_all(fd, first);
    send_all(fd, "\nSET message hello world\nSET session Rupdip 1\nGET message\nGET session\nDEL message\nGET message\n");
    const std::string expected = "PONG\nOK\nOK\nhello world\nRupdip\n(integer) 1\n(nil)\n";
    check(read_at_least(fd, expected.size()).substr(0, expected.size()) == expected, "pipeline and partial reads");
    send_all(fd, "SET invalid value -1\n");
    const std::string invalid_ttl = "(error) invalid TTL\n";
    check(read_at_least(fd, invalid_ttl.size()).find(invalid_ttl) == 0, "invalid TTL response");
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    send_all(fd, "GET session\n");
    check(read_at_least(fd, 6).find("(nil)\n") == 0, "network TTL expiration");
    send_all(fd, "QUIT\n");
    check(read_at_least(fd, 4).find("BYE\n") == 0, "quit response");
    close(fd);

    std::vector<std::thread> clients;
    std::atomic<bool> client_success{true};
    for (int client = 0; client < 8; ++client) {
        clients.emplace_back([client, port, &client_success] {
            const int socket_fd = connect_to(port);
            if (socket_fd < 0) { client_success = false; return; }
            const std::string key = "client_" + std::to_string(client);
            send_all(socket_fd, "SET " + key + " value\nGET " + key + "\n");
            const std::string result = read_at_least(socket_fd, 9);
            if (result.substr(0, 9) != "OK\nvalue\n") client_success = false;
            close(socket_fd);
        });
    }
    for (auto& client : clients) client.join();
    check(client_success.load(), "concurrent client sessions");

    kill(child, SIGINT);
    int status = 0;
    waitpid(child, &status, 0);
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "clean server shutdown");
}

void stress_tests(const char* server_path) {
    constexpr int port = 6392;
    const pid_t child = fork();
    check(child >= 0, "fork stress server");
    if (child == 0) {
        execl(server_path, server_path, "6392", "4", static_cast<char*>(nullptr));
        _exit(127);
    }
    std::atomic<bool> successful{true};
    std::vector<std::thread> clients;
    for (int client = 0; client < 20; ++client) {
        clients.emplace_back([client, port, &successful] {
            const int fd = connect_to(port);
            if (fd < 0) { successful = false; return; }
            for (int operation = 0; operation < 100 && successful; ++operation) {
                const std::string key = "stress_" + std::to_string(client);
                send_all(fd, "SET " + key + " " + std::to_string(operation) + "\n");
                if (read_line(fd) != "OK\n") successful = false;
                send_all(fd, "GET " + key + "\n");
                const std::string value = read_line(fd);
                if (value.empty() || value.back() != '\n') successful = false;
                send_all(fd, "DEL " + key + "\n");
                if (read_line(fd) != "(integer) 1\n") successful = false;
            }
            close(fd);
        });
    }
    for (auto& client : clients) client.join();
    check(successful.load(), "stress operations");
    kill(child, SIGINT);
    int status = 0;
    waitpid(child, &status, 0);
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "stress server shutdown");
}

void run_benchmark(const char* benchmark_path, const char* pipeline, const char* clients,
                   const char* requests, const char* workload) {
    const pid_t child = fork();
    check(child >= 0, "fork benchmark");
    if (child == 0) {
        execl(benchmark_path, benchmark_path,
              "--host", "127.0.0.1", "--port", "6393",
              "--clients", clients, "--requests", requests,
              "--pipeline", pipeline, "--workload", workload,
              "--key-space", "100", "--value-size", "16",
              "--timeout", "1000", static_cast<char*>(nullptr));
        _exit(127);
    }
    int status = 0;
    check(waitpid(child, &status, 0) == child, "wait for benchmark");
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "benchmark completed without failures");
}

void benchmark_tests(const char* server_path, const char* benchmark_path) {
    constexpr int port = 6393;
    const pid_t child = fork();
    check(child >= 0, "fork benchmark server");
    if (child == 0) {
        execl(server_path, server_path, "6393", "4", static_cast<char*>(nullptr));
        _exit(127);
    }
    const int readiness_fd = connect_to(port);
    check(readiness_fd >= 0, "connect benchmark server");
    close(readiness_fd);
    run_benchmark(benchmark_path, "1", "4", "400", "mixed");
    run_benchmark(benchmark_path, "8", "8", "800", "set-get-del");
    kill(child, SIGINT);
    int status = 0;
    waitpid(child, &status, 0);
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "benchmark server shutdown");
}
}

int main(int argc, char* argv[]) {
    if (argc < 2) return 2;
    if (std::string(argv[1]) == "unit") unit_tests();
    else if (std::string(argv[1]) == "integration" && argc == 3) integration_tests(argv[2]);
    else if (std::string(argv[1]) == "stress" && argc == 3) stress_tests(argv[2]);
    else if (std::string(argv[1]) == "benchmark" && argc == 4) benchmark_tests(argv[2], argv[3]);
    else return 2;
    std::cout << "PASS\n";
    return 0;
}
