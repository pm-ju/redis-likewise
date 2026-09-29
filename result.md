# Benchmark Results

Generated from the Release build on 2026-08-20 in WSL Ubuntu. The server and benchmark ran on the same machine over localhost TCP.

## Validation

```text
Command: cmake --build build-release -- -j2
Result: PASS

Command: ctest --test-dir build-release -R kv_benchmark_tests --output-on-failure
Result: PASS
1/1 test passed
```

The benchmark-specific CTest launches the real `kv_server` and exercises both non-pipelined and pipelined workloads.

## Matrix Command

The complete matrix was run with:

```bash
REQUESTS=200 WARMUP=0 PORT_BASE=6600 \
RESULTS_DIR=/tmp/kv-benchmark-results-pass \
BENCHMARK_TIMEOUT=30 bash scripts/run_benchmarks.sh
```

Configuration:

- Build type: Release
- Compiler: GCC 15.2.0
- Host: `127.0.0.1`
- Workload: `mixed` = 70% GET, 20% SET, 10% DEL
- Key space: 1000
- Value size: 16 bytes
- Warmup: 0 seconds
- Requests per row: 200
- Workers: 1 and 4
- Clients: 1, 8, and 16
- Pipeline depth: 1 and 4
- Completed CSV files: 12
- Failures in successful matrix: 0

## Results

Latency values are microseconds. Throughput is completed requests per second. Every row below completed 200/200 requests.

| Workers | Clients | Pipeline | Requests | Successes | Failures | Throughput req/s | Average us | P50 us | P95 us | P99 us |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 1 | 1 | 200 | 200 | 0 | 3857.44 | 252.315 | 237.5 | 384.624 | 482.197 |
| 1 | 1 | 4 | 200 | 200 | 0 | 92.7229 | 21775.4 | 887.447 | 44332.6 | 44600.6 |
| 1 | 8 | 1 | 200 | 200 | 0 | 3143.06 | 1358.68 | 282.01 | 665.328 | 37491.6 |
| 1 | 8 | 4 | 200 | 200 | 0 | 113.654 | 142477 | 43135.4 | 1098930 | 1539300 |
| 1 | 16 | 1 | 200 | 200 | 0 | 2354.34 | 3560.71 | 398.695 | 26983.9 | 69916 |
| 1 | 16 | 4 | 200 | 200 | 0 | 142.059 | 228050 | 43270 | 1143490 | 1320250 |
| 4 | 1 | 1 | 200 | 200 | 0 | 3392.65 | 287.67 | 252.428 | 517.038 | 673.379 |
| 4 | 1 | 4 | 200 | 200 | 0 | 92.6932 | 21818.8 | 1078.75 | 44366.6 | 44586.3 |
| 4 | 8 | 1 | 200 | 200 | 0 | 13251.4 | 377.999 | 253.389 | 453.141 | 6055.44 |
| 4 | 8 | 4 | 200 | 200 | 0 | 451.403 | 36360.4 | 1654.11 | 218967 | 222093 |
| 4 | 16 | 1 | 200 | 200 | 0 | 13337.2 | 685.897 | 238.473 | 3876.75 | 9480.11 |
| 4 | 16 | 4 | 200 | 200 | 0 | 483.065 | 62224.4 | 43278.8 | 262812 | 321753 |

## Observations

This was a short localhost matrix, not a production capacity claim. In this run, pipeline depth 4 had substantially lower throughput and higher tail latency than pipeline depth 1. The table reports measured behavior; any explanation is only a hypothesis. A likely contributor is the server's one-worker-per-complete-connection model combined with the benchmark's batch-completion latency.

The 4-worker, 8-client, pipeline-1 row reached the highest measured throughput in this run: 13,251.4 requests/sec. The 4-worker, 16-client, pipeline-1 row was similar at 13,337.2 requests/sec, with higher tail latency.

## Additional Failed Attempt

A separate run with `REQUESTS=500` reached the configuration `workers=1, clients=8, pipeline=4` and produced:

- 268 completed requests
- 16 timeout failures
- 95.4837 requests/sec
- p99 latency: 1,978,640 microseconds

The server shut down cleanly and no processes remained. This case is retained as an observed timeout under the heavier request count, not presented as a successful result.

## Cleanup

The completed matrix generated 12 CSV files under `/tmp/kv-benchmark-results-pass`. Process checks after the run found no remaining `kv_server` or `kv_benchmark` processes.
