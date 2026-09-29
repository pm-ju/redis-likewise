#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <netdb.h>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::size_t kFailureKinds = 6;
enum class FailureKind { Connection, Send, Receive, Malformed, Unexpected, Timeout };

std::size_t failure_index(FailureKind kind) { return static_cast<std::size_t>(kind); }

const char* failure_name(FailureKind kind) {
    switch (kind) {
    case FailureKind::Connection: return "connection";
    case FailureKind::Send: return "send";
    case FailureKind::Receive: return "receive";
    case FailureKind::Malformed: return "malformed";
    case FailureKind::Unexpected: return "unexpected";
    case FailureKind::Timeout: return "timeout";
    }
    return "unknown";
}

struct BenchmarkConfig {
    std::string host = "127.0.0.1";
    std::uint16_t port = 6380;
    std::size_t clients = 1;
    std::size_t requests = 10000;
    std::size_t pipeline = 1;
    std::string workload = "mixed";
    std::size_t key_space = 1000;
    std::size_t value_size = 16;
    std::size_t warmup_seconds = 0;
    std::size_t duration_seconds = 0;
    std::uint64_t seed = 42;
    std::size_t timeout_ms = 2000;
    std::string output;
    std::string format = "text";
    std::string server_workers = "unknown";
    std::string build_type = "unknown";
};

struct Request {
    std::string command;
    enum class Kind { Set, Get, Del } kind;
};

struct ClientCounts {
    std::size_t completed = 0;
    std::size_t failed = 0;
    std::array<std::size_t, kFailureKinds> failures{};
    std::vector<double> latency_us;
};

struct BenchmarkResult {
    std::size_t total = 0;
    std::size_t completed = 0;
    std::size_t failed = 0;
    std::array<std::size_t, kFailureKinds> failures{};
    double duration_seconds = 0.0;
    double min_latency_us = 0.0;
    double average_latency_us = 0.0;
    double p50_latency_us = 0.0;
    double p95_latency_us = 0.0;
    double p99_latency_us = 0.0;
    double max_latency_us = 0.0;
    std::vector<double> latencies;
};

class Socket {
public:
    Socket() = default;
    ~Socket() { close(); }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    bool connect_to(const std::string& host, std::uint16_t port, std::size_t timeout_ms) {
        close();
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        const std::string service = std::to_string(port);
        addrinfo* addresses = nullptr;
        if (getaddrinfo(host.c_str(), service.c_str(), &hints, &addresses) != 0) return false;
        for (addrinfo* address = addresses; address != nullptr; address = address->ai_next) {
            const int candidate = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
            if (candidate < 0) continue;
            if (::connect(candidate, address->ai_addr, address->ai_addrlen) == 0) {
                fd_ = candidate;
                timeval timeout{static_cast<time_t>(timeout_ms / 1000),
                                 static_cast<suseconds_t>((timeout_ms % 1000) * 1000)};
                setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
                setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
                freeaddrinfo(addresses);
                return true;
            }
            ::close(candidate);
        }
        freeaddrinfo(addresses);
        return false;
    }

    bool send_all(const std::string& data, FailureKind& failure) {
        std::size_t sent = 0;
        while (sent < data.size()) {
            const ssize_t count = send(fd_, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
            if (count < 0 && errno == EINTR) continue;
            if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                failure = FailureKind::Timeout;
                return false;
            }
            if (count <= 0) {
                failure = FailureKind::Send;
                return false;
            }
            sent += static_cast<std::size_t>(count);
        }
        return true;
    }

    bool read_line(std::string& line, FailureKind& failure) {
        while (true) {
            const std::size_t newline = receive_buffer_.find('\n');
            if (newline != std::string::npos) {
                line = receive_buffer_.substr(0, newline);
                receive_buffer_.erase(0, newline + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                return true;
            }
            if (receive_buffer_.size() > 8192) {
                failure = FailureKind::Malformed;
                return false;
            }
            char buffer[4096];
            const ssize_t count = recv(fd_, buffer, sizeof(buffer), 0);
            if (count < 0 && errno == EINTR) continue;
            if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                failure = FailureKind::Timeout;
                return false;
            }
            if (count <= 0) {
                failure = FailureKind::Receive;
                return false;
            }
            receive_buffer_.append(buffer, static_cast<std::size_t>(count));
        }
    }

private:
    void close() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
        receive_buffer_.clear();
    }

    int fd_ = -1;
    std::string receive_buffer_;
};

std::string usage() {
    return "Usage: kv_benchmark [options]\n"
           "  --host HOST             Server host (default: 127.0.0.1)\n"
           "  --port PORT             Server port (default: 6380)\n"
           "  --clients N             Concurrent TCP clients (default: 1)\n"
           "  --requests N            Measured requests, ignored when duration is set (default: 10000)\n"
           "  --pipeline N            Commands sent before responses are read (default: 1)\n"
           "  --workload NAME         set-heavy, get-heavy, mixed, set-get, or set-get-del\n"
           "  --key-space N           Number of reusable keys (default: 1000)\n"
           "  --value-size N          Generated value bytes (default: 16)\n"
           "  --warmup N              Warmup seconds excluded from results (default: 0)\n"
           "  --duration N            Measurement seconds; takes precedence over requests\n"
           "  --seed N                Deterministic workload seed (default: 42)\n"
           "  --timeout N             Socket timeout milliseconds (default: 2000)\n"
           "  --server-workers N      Metadata for the server launched separately\n"
           "  --build-type NAME       Metadata such as Release or Debug\n"
           "  --output PATH           Write the selected report to a file\n"
           "  --format text|csv       Report format (default: text)\n"
           "  --help                  Show this help\n";
}

std::size_t parse_size(const std::string& value, const char* name, std::size_t maximum) {
    try {
        std::size_t consumed = 0;
        const unsigned long long parsed = std::stoull(value, &consumed);
        if (consumed != value.size() || parsed == 0 || parsed > maximum) throw std::out_of_range(name);
        return static_cast<std::size_t>(parsed);
    } catch (...) {
        throw std::invalid_argument(std::string(name) + " must be an integer from 1 to " + std::to_string(maximum));
    }
}

std::size_t parse_nonnegative(const std::string& value, const char* name, std::size_t maximum) {
    if (value == "0") return 0;
    return parse_size(value, name, maximum);
}

std::uint64_t parse_seed(const std::string& value) {
    try {
        std::size_t consumed = 0;
        const unsigned long long parsed = std::stoull(value, &consumed);
        if (consumed != value.size()) throw std::invalid_argument("seed");
        return static_cast<std::uint64_t>(parsed);
    } catch (...) {
        throw std::invalid_argument("seed must be a non-negative integer");
    }
}

void set_option(BenchmarkConfig& config, const std::string& option, const std::string& value) {
    if (option == "--host") config.host = value;
    else if (option == "--port") config.port = static_cast<std::uint16_t>(parse_size(value, "port", 65535));
    else if (option == "--clients") config.clients = parse_size(value, "clients", 256);
    else if (option == "--requests") config.requests = parse_nonnegative(value, "requests", 1000000000);
    else if (option == "--pipeline") config.pipeline = parse_size(value, "pipeline", 1024);
    else if (option == "--workload") config.workload = value;
    else if (option == "--key-space") config.key_space = parse_size(value, "key-space", 1000000000);
    else if (option == "--value-size") config.value_size = parse_size(value, "value-size", 2048);
    else if (option == "--warmup") config.warmup_seconds = parse_nonnegative(value, "warmup", 86400);
    else if (option == "--duration") config.duration_seconds = parse_nonnegative(value, "duration", 86400);
    else if (option == "--seed") config.seed = parse_seed(value);
    else if (option == "--timeout") config.timeout_ms = parse_size(value, "timeout", 600000);
    else if (option == "--server-workers") config.server_workers = value;
    else if (option == "--build-type") config.build_type = value;
    else if (option == "--output") config.output = value;
    else if (option == "--format") config.format = value;
    else throw std::invalid_argument("unknown option: " + option);
}

BenchmarkConfig parse_config(int argc, char* argv[]) {
    BenchmarkConfig config;
    for (int index = 1; index < argc; ++index) {
        const std::string option = argv[index];
        if (option == "--help") {
            std::cout << usage();
            std::exit(0);
        }
        if (index + 1 >= argc || option.rfind("--", 0) != 0) throw std::invalid_argument("expected option and value: " + option);
        set_option(config, option, argv[++index]);
    }
    const std::array<std::string, 5> workloads{"set-heavy", "get-heavy", "mixed", "set-get", "set-get-del"};
    if (std::find(workloads.begin(), workloads.end(), config.workload) == workloads.end())
        throw std::invalid_argument("workload must be set-heavy, get-heavy, mixed, set-get, or set-get-del");
    if (config.format != "text" && config.format != "csv") throw std::invalid_argument("format must be text or csv");
    if (config.duration_seconds == 0 && config.requests == 0) throw std::invalid_argument("requests or duration is required");
    return config;
}

std::string make_value(std::size_t client, std::size_t key, std::size_t size) {
    std::string value = "value-" + std::to_string(client) + "-" + std::to_string(key);
    if (value.size() < size) value.append(size - value.size(), 'x');
    return value.substr(0, size);
}

Request make_request(const BenchmarkConfig& config, std::size_t client, std::mt19937_64& random) {
    const std::size_t key_number = static_cast<std::size_t>(random() % config.key_space);
    const std::string key = "bench-key-" + std::to_string(key_number);
    std::uniform_int_distribution<int> distribution(0, 99);
    const int choice = distribution(random);
    if ((config.workload == "set-heavy" && choice < 80) || (config.workload == "set-get" && choice < 50) ||
        (config.workload == "mixed" && choice >= 70 && choice < 90) ||
        (config.workload == "set-get-del" && choice >= 70 && choice < 90)) {
        return {"SET " + key + " " + make_value(client, key_number, config.value_size) + "\n", Request::Kind::Set};
    }
    if ((config.workload == "get-heavy" && choice < 90) || (config.workload == "set-heavy" && choice >= 80) ||
        (config.workload == "set-get" && choice >= 50) || (config.workload == "mixed" && choice < 70) ||
        (config.workload == "set-get-del" && choice < 70)) {
        return {"GET " + key + "\n", Request::Kind::Get};
    }
    return {"DEL " + key + "\n", Request::Kind::Del};
}

bool valid_response(const Request& request, const std::string& response) {
    if (request.kind == Request::Kind::Set) return response == "OK";
    if (request.kind == Request::Kind::Del) return response == "(integer) 0" || response == "(integer) 1";
    if (response == "(nil)") return true;
    return !response.empty() && response.find('\r') == std::string::npos && response.find('\n') == std::string::npos;
}

void count_failure(ClientCounts& counts, FailureKind kind, std::size_t amount = 1) {
    counts.failed += amount;
    counts.failures[failure_index(kind)] += amount;
}

void wait_until(Clock::time_point time) {
    std::this_thread::sleep_until(time);
}

void run_client_phase(const BenchmarkConfig& config, std::size_t client, std::size_t request_count,
                      Clock::time_point start, Clock::time_point deadline, bool measure, ClientCounts& counts) {
    Socket socket;
    if (!socket.connect_to(config.host, config.port, config.timeout_ms)) {
        if (measure) count_failure(counts, FailureKind::Connection, request_count == 0 ? 1 : request_count);
        return;
    }
    wait_until(start);
    std::mt19937_64 random(config.seed + client * 1000003ULL + (measure ? 17ULL : 0ULL));
    std::size_t completed_requests = 0;
    while ((request_count == 0 || completed_requests < request_count) &&
           (deadline == Clock::time_point::max() || Clock::now() < deadline)) {
        std::vector<Request> batch;
        const std::size_t remaining = request_count == 0 ? config.pipeline : request_count - completed_requests;
        const std::size_t batch_size = std::min(config.pipeline, remaining);
        batch.reserve(batch_size);
        for (std::size_t index = 0; index < batch_size; ++index) batch.push_back(make_request(config, client, random));
        const auto sent_at = Clock::now();
        FailureKind failure = FailureKind::Send;
        bool sent = true;
        for (const Request& request : batch) {
            if (!socket.send_all(request.command, failure)) {
                sent = false;
                break;
            }
        }
        if (!sent) {
            if (measure) count_failure(counts, failure, batch.size());
            return;
        }
        for (const Request& request : batch) {
            std::string response;
            if (!socket.read_line(response, failure)) {
                if (measure) count_failure(counts, failure, batch.size());
                return;
            }
            if (!measure) continue;
            if (!valid_response(request, response)) {
                count_failure(counts, response.empty() ? FailureKind::Malformed : FailureKind::Unexpected);
                continue;
            }
            ++counts.completed;
            const double latency = std::chrono::duration<double, std::micro>(Clock::now() - sent_at).count();
            counts.latency_us.push_back(latency);
        }
        completed_requests += batch.size();
    }
    if (measure && request_count != 0 && completed_requests < request_count) {
        count_failure(counts, FailureKind::Timeout, request_count - completed_requests);
    }
}

BenchmarkResult run_benchmark(const BenchmarkConfig& config) {
    std::vector<std::thread> warmup_threads;
    const auto warmup_start = Clock::now() + std::chrono::milliseconds(50);
    const auto warmup_deadline = warmup_start + std::chrono::seconds(config.warmup_seconds);
    if (config.warmup_seconds > 0) {
        for (std::size_t client = 0; client < config.clients; ++client) {
            warmup_threads.emplace_back([&, client] {
                ClientCounts ignored;
                run_client_phase(config, client, 0, warmup_start, warmup_deadline, false, ignored);
            });
        }
        for (auto& thread : warmup_threads) thread.join();
    }

    const auto measurement_start = Clock::now() + std::chrono::milliseconds(50);
    const auto measurement_deadline = config.duration_seconds == 0
        ? Clock::time_point::max()
        : measurement_start + std::chrono::seconds(config.duration_seconds);
    std::vector<ClientCounts> client_counts(config.clients);
    std::vector<std::thread> measurement_threads;
    const std::size_t base_requests = config.duration_seconds == 0 ? config.requests / config.clients : 0;
    const std::size_t extra_requests = config.duration_seconds == 0 ? config.requests % config.clients : 0;
    for (std::size_t client = 0; client < config.clients; ++client) {
        const std::size_t assigned = config.duration_seconds == 0 ? base_requests + (client < extra_requests ? 1 : 0) : 0;
        measurement_threads.emplace_back([&, client, assigned] {
            run_client_phase(config, client, assigned, measurement_start, measurement_deadline, true, client_counts[client]);
        });
    }
    for (auto& thread : measurement_threads) thread.join();
    const auto measurement_end = Clock::now();

    BenchmarkResult result;
    result.duration_seconds = std::chrono::duration<double>(measurement_end - measurement_start).count();
    for (const ClientCounts& counts : client_counts) {
        result.completed += counts.completed;
        result.failed += counts.failed;
        result.failures[0] += counts.failures[0];
        result.failures[1] += counts.failures[1];
        result.failures[2] += counts.failures[2];
        result.failures[3] += counts.failures[3];
        result.failures[4] += counts.failures[4];
        result.failures[5] += counts.failures[5];
        result.latencies.insert(result.latencies.end(), counts.latency_us.begin(), counts.latency_us.end());
    }
    result.total = result.completed + result.failed;
    if (!result.latencies.empty()) {
        std::sort(result.latencies.begin(), result.latencies.end());
        result.min_latency_us = result.latencies.front();
        result.max_latency_us = result.latencies.back();
        double sum = 0.0;
        for (double latency : result.latencies) sum += latency;
        result.average_latency_us = sum / static_cast<double>(result.latencies.size());
        const auto percentile = [&](double fraction) {
            const std::size_t rank = static_cast<std::size_t>(std::ceil(fraction * result.latencies.size()));
            return result.latencies[std::max<std::size_t>(1, rank) - 1];
        };
        result.p50_latency_us = percentile(0.50);
        result.p95_latency_us = percentile(0.95);
        result.p99_latency_us = percentile(0.99);
    }
    return result;
}

std::string workload_distribution(const std::string& workload) {
    if (workload == "set-heavy") return "SET 80%, GET 20%";
    if (workload == "get-heavy") return "GET 90%, SET 10%";
    if (workload == "set-get") return "SET 50%, GET 50%";
    if (workload == "set-get-del") return "GET 70%, SET 20%, DEL 10%";
    return "GET 70%, SET 20%, DEL 10%";
}

std::string now_timestamp() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
    localtime_r(&time, &local);
    std::ostringstream output;
    output << std::put_time(&local, "%Y-%m-%dT%H:%M:%S");
    return output.str();
}

void print_text(const BenchmarkConfig& config, const BenchmarkResult& result, std::ostream& output) {
    const auto milliseconds = [](double microseconds) { return microseconds / 1000.0; };
    output << "========================================\nKV BENCHMARK\n========================================\n"
           << "Server:\n  Host: " << config.host << "\n  Port: " << config.port
           << "\n  Workers: " << config.server_workers << "\n  Build type: " << config.build_type
           << "\n\nWorkload:\n  Type: " << config.workload << "\n  Distribution: " << workload_distribution(config.workload)
           << "\n  Key space: " << config.key_space << "\n  Value size: " << config.value_size << " bytes\n"
           << "\nClients:\n  Connections: " << config.clients << "\n  Pipeline: " << config.pipeline
           << "\n\nWarmup:\n  Duration: " << config.warmup_seconds << " sec\nMeasurement:\n  Requested requests: "
           << (config.duration_seconds == 0 ? std::to_string(config.requests) : "duration mode")
           << "\n  Duration: " << std::fixed << std::setprecision(3) << result.duration_seconds << " sec\n\nResults:\n"
           << "  Requests: " << result.total << "\n  Successful: " << result.completed << "\n  Failed: " << result.failed
           << "\n  Throughput: " << (result.duration_seconds > 0 ? result.completed / result.duration_seconds : 0.0) << " req/s\n"
           << "\nLatency (validated responses; pipeline latency is batch completion latency):\n"
           << "  Min: " << result.min_latency_us << " us (" << milliseconds(result.min_latency_us) << " ms)\n"
           << "  Avg: " << result.average_latency_us << " us (" << milliseconds(result.average_latency_us) << " ms)\n"
           << "  P50: " << result.p50_latency_us << " us (" << milliseconds(result.p50_latency_us) << " ms)\n"
           << "  P95: " << result.p95_latency_us << " us (" << milliseconds(result.p95_latency_us) << " ms)\n"
           << "  P99: " << result.p99_latency_us << " us (" << milliseconds(result.p99_latency_us) << " ms)\n"
           << "  Max: " << result.max_latency_us << " us (" << milliseconds(result.max_latency_us) << " ms)\n\nFailures:\n";
    for (std::size_t index = 0; index < kFailureKinds; ++index)
        output << "  " << failure_name(static_cast<FailureKind>(index)) << ": " << result.failures[index] << "\n";
    output << "========================================\n";
}

void print_csv(const BenchmarkConfig& config, const BenchmarkResult& result, std::ostream& output) {
    output << "timestamp,host,port,workers,build_type,clients,pipeline,workload,key_space,value_size,warmup_seconds,duration_seconds,requests,successes,failures,throughput,min_latency_us,avg_latency_us,p50_latency_us,p95_latency_us,p99_latency_us,max_latency_us,connection_failures,send_failures,receive_failures,malformed_failures,unexpected_failures,timeout_failures\n";
    output << now_timestamp() << ',' << config.host << ',' << config.port << ',' << config.server_workers << ',' << config.build_type << ','
           << config.clients << ',' << config.pipeline << ',' << config.workload << ',' << config.key_space << ',' << config.value_size << ','
           << config.warmup_seconds << ',' << result.duration_seconds << ',' << result.total << ',' << result.completed << ',' << result.failed << ','
           << (result.duration_seconds > 0 ? result.completed / result.duration_seconds : 0.0) << ',' << result.min_latency_us << ','
           << result.average_latency_us << ',' << result.p50_latency_us << ',' << result.p95_latency_us << ',' << result.p99_latency_us << ','
           << result.max_latency_us;
    for (std::size_t failure : result.failures) output << ',' << failure;
    output << '\n';
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        const BenchmarkConfig config = parse_config(argc, argv);
        const BenchmarkResult result = run_benchmark(config);
        std::ofstream file;
        std::ostream* output = &std::cout;
        if (!config.output.empty()) {
            file.open(config.output);
            if (!file) throw std::runtime_error("could not open output: " + config.output);
            output = &file;
        }
        if (config.format == "csv") print_csv(config, result, *output);
        else print_text(config, result, *output);
        return result.failed == 0 ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "[ERROR] " << error.what() << "\n" << usage();
        return 2;
    }
}
