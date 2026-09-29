# System Architecture

Phase 3 adds `kv_benchmark` as a standalone TCP load generator. It does not link to `kv_core` or call `KvStore` directly. Every measured operation travels through a real socket to `kv_server`, so the measured path includes connection handling, the bounded worker queue, command parsing, storage synchronization, and response framing.

The benchmark creates one persistent TCP connection and one C++ thread per configured client. Each client generates deterministic keys from a reusable key space and sends one request at a time when `--pipeline 1`, or a batch of commands before reading ordered responses for larger pipeline depths. A request is successful only after its response is received and validated.

Warmup and measurement are separate phases. Warmup connections and samples are discarded. Measurement records monotonic `steady_clock` latency for each validated response; with pipelining, each response in a batch receives the batch send-to-response-completion latency. Percentiles use the nearest-rank method after sorting samples: rank `ceil(p * N)` for percentile `p`.

The benchmark reports throughput, total/completed/failed requests, categorized failures, and min/average/p50/p95/p99/max latency. `--format csv` writes machine-readable results without adding a third-party dependency. The benchmark's `--server-workers` and `--build-type` options are metadata; the server is configured separately with `kv_server [port] [workers]`.

`scripts/run_benchmarks.sh` builds a Release tree, starts only the server processes it owns, waits for TCP readiness with a bounded attempt count, runs a representative matrix over workers 1/4, clients 1/8/16, and pipeline depths 1/4, then shuts down each captured PID and writes CSV files under `benchmarks/results/`. Each worker configuration uses a distinct port derived from `PORT_BASE`, each benchmark invocation is bounded by `BENCHMARK_TIMEOUT`, and the EXIT/INT/TERM trap cleans up the currently owned server. `RESULTS_DIR` can redirect output to a writable WSL path such as `/tmp/kv-benchmark-results`.

The first runner stopped at the transition after the one-worker rows without creating the four-worker log. Its first server had stopped and no process was orphaned, but the old fixed-port, implicit-error lifecycle did not identify the boundary. The final runner makes that boundary explicit with per-worker ports, readiness checks that detect early server exit, benchmark timeouts, failure messages naming the worker/client/pipeline case, and captured-PID cleanup.

## Server Architecture

## Lifecycle and ownership

```text
accept loop
    |
    +-- accepted fd remains owned by producer
    |
    +-- submit(fd) succeeds --> one worker task owns fd
    |                              |
    |                              +-- ClientSession reads and processes
    |                              +-- SocketGuard closes fd exactly once
    |
    +-- queue full --> busy response, producer closes fd
```

`Server` creates the listening socket, accepts clients, and submits one task per accepted socket. A task runs the complete `ClientSession` loop. No command from a given socket is submitted separately, so that connection's command order is preserved without per-connection sequencing locks. Different sockets execute in parallel on different pool workers.

The task queue is bounded at 64 pending sockets. Submission is non-blocking; a full queue is reported to the client and the descriptor is closed. This is the backpressure policy for Phase 2.

## Worker pool

`ThreadPool` owns a configurable vector of `std::thread` objects, a mutex-protected `std::queue<std::function<void()>>`, and a condition variable. Workers sleep while the queue is empty, wake for work or shutdown, execute tasks outside the queue lock, and catch task exceptions at the worker boundary. Shutdown sets a stop flag, wakes all workers, drains queued tasks, and joins every thread. No worker is detached.

## Command and storage flow

```text
TCP bytes -> ClientSession receive buffer -> complete line
         -> CommandParser -> Command -> KvStore
         -> newline-terminated response
```

The existing line buffer handles partial reads, multiple commands per read, and CRLF. Since a session task processes its stream in one loop, pipelined commands are answered in input order.

`KvStore` stores `Entry { value, expires_at }` in `std::unordered_map<std::string, Entry>`. A `std::shared_mutex` protects the map. `SET` and `DEL` use exclusive locking. `GET` also uses an exclusive lock because it may lazily erase an expired entry; this avoids a check-then-delete race and keeps the policy simple.

The map is declared `mutable` because `get` remains logically const for callers while lazy expiration removes expired entries. The exclusive lock establishes the required mutation boundary; no `const_cast` is used. Parser command aggregates initialize all fields explicitly so strict compiler warnings remain clean.

TTL uses `std::chrono::steady_clock`. `SET key value ttl` stores an expiration timestamp rather than sleeping in a worker. TTL zero is immediately expired, negative, overflowing, and greater-than-ten-year numeric values are rejected. A non-numeric final token remains value text to preserve Phase 1 values containing spaces. Missing or expired entries are treated as `(nil)` / delete count zero.

The parser preserves Phase 1 values containing spaces. A final numeric token is recognized as TTL, so `SET message hello world` remains a value with spaces while `SET session Rupdip 10` uses TTL. This syntax tradeoff is documented in the README.

## Shutdown

SIGINT sets a signal-safe process flag. The accept loop stops accepting, closes the listening descriptor, and shuts down the pool. Active sessions use a short receive timeout and check the flag; queued tasks also exit promptly when they begin. The pool joins all workers before the server returns. Socket ownership is always local to either the producer's rejection path or a worker's RAII guard.

## Limits and future work

The request-line limit is 4096 bytes, key limit is 256 bytes, value limit is 2048 bytes, and task queue limit is 64. Phase 2 does not include persistence, authentication, clustering, epoll, asynchronous I/O, or benchmarking. Phase 3 may add load generation, latency percentiles, and performance tuning.
