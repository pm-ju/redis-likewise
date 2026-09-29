#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BUILD_DIR=${BUILD_DIR:-"$ROOT_DIR/build-release"}
RESULTS_DIR=${RESULTS_DIR:-"$ROOT_DIR/benchmarks/results"}
PORT_BASE=${PORT_BASE:-6394}
REQUESTS=${REQUESTS:-5000}
WARMUP=${WARMUP:-1}
READINESS_ATTEMPTS=${READINESS_ATTEMPTS:-100}
BENCHMARK_TIMEOUT=${BENCHMARK_TIMEOUT:-60}

cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR" -- -j2
mkdir -p "$RESULTS_DIR"

server_pid=""
server_port=""
cleanup() {
    if [[ -n "$server_pid" ]] && kill -0 "$server_pid" 2>/dev/null; then
        kill -INT "$server_pid" 2>/dev/null || true
        wait "$server_pid" 2>/dev/null || true
    fi
    server_pid=""
}
trap cleanup EXIT INT TERM

wait_for_server() {
    for ((attempt = 1; attempt <= READINESS_ATTEMPTS; ++attempt)); do
        if ! kill -0 "$server_pid" 2>/dev/null; then
            echo "Server exited before readiness; see $1" >&2
            return 1
        fi
        if (exec 3<>"/dev/tcp/127.0.0.1/$server_port") 2>/dev/null; then
            exec 3>&-
            exec 3<&-
            return 0
        fi
        sleep 0.02
    done
    echo "Timed out waiting for server on port $server_port; see $1" >&2
    return 1
}

for workers in 1 4; do
    server_port=$((PORT_BASE + workers))
    server_log="$RESULTS_DIR/server-${workers}.log"
    "$BUILD_DIR/kv_server" "$server_port" "$workers" >"$server_log" 2>&1 &
    server_pid=$!
    wait_for_server "$server_log"
    for clients in 1 8 16; do
        for pipeline in 1 4; do
            output="$RESULTS_DIR/mixed-w${workers}-c${clients}-p${pipeline}.csv"
            if ! timeout --signal=TERM "${BENCHMARK_TIMEOUT}s" "$BUILD_DIR/kv_benchmark" \
                --host 127.0.0.1 --port "$server_port" \
                --server-workers "$workers" --build-type Release \
                --clients "$clients" --requests "$REQUESTS" \
                --pipeline "$pipeline" --workload mixed \
                --key-space 1000 --value-size 16 \
                --warmup "$WARMUP" --timeout 2000 \
                --format csv --output "$output"; then
                echo "Benchmark failed or timed out: workers=$workers clients=$clients pipeline=$pipeline" >&2
                exit 1
            fi
        done
    done
    kill -INT "$server_pid"
    wait "$server_pid"
    server_pid=""
    server_port=""
done

printf 'Benchmark CSV files written to %s\n' "$RESULTS_DIR"
