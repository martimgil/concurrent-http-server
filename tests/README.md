# Concurrent HTTP Server - Test Suite

This directory contains all automated tests to validate the functionality, performance, and reliability of the concurrent HTTP server.

## Prerequisites

Before running the tests, ensure the following tools are installed:

| Tool | Package (Debian/Ubuntu) | Purpose |
|------|-------------------------|---------|
| Apache Bench (ab) | `apache2-utils` | Load and performance testing |
| curl | `curl` | Basic HTTP requests |
| Valgrind | `valgrind` | Memory leak and race condition detection |
| GCC | `build-essential` | Compilation of C test programs |
| bc | `bc` | Mathematical calculations in bash |

### Installation

```bash
sudo apt update
sudo apt install apache2-utils curl valgrind build-essential bc
```

---

## Directory Structure

| File | Description |
|------|-------------|
| `test_suite.sh` | Main test script with all automated tests |
| `stress_test.sh` | Extended stress test (5+ minutes of continuous load) |
| `test_concurrent.c` | Multi-threaded cache consistency test |
| `stress_client.c` | Client to saturate the server's connection queue |

---

## Running the Tests

### Complete Test Suite

Executes all tests automatically in 4 different modes:

```bash
cd /path/to/project
./tests/test_suite.sh
```

The script automatically compiles the server and creates the necessary test files in the `www/` directory.

### Extended Stress Test

Runs a continuous load test for 5 minutes:

```bash
./tests/stress_test.sh
```

---

## Test Modes

The `test_suite.sh` script automatically runs tests in 4 modes:

| Mode | Description | Objective |
|------|-------------|-----------|
| Normal | No instrumentation | Basic functional validation |
| Helgrind | Valgrind with Helgrind | Race condition detection |
| TSan | Thread Sanitizer | Data race detection (compiled with `-fsanitize=thread`) |
| Memcheck | Valgrind Memcheck | Memory leak detection |

---

## Test Coverage

### Functional HTTP Tests

- GET `/index.html` returns 200 OK
- GET on non-existent file returns 404 Not Found
- GET `/` serves `index.html` automatically (directory index)
- MIME type verification (`.html`, `.css`, `.js`, `.png`)

### HTTP Status Code Tests

- 403 Forbidden: Files without read permission
- 500 Internal Server Error: Internal server failures

### Load Tests

- Apache Bench test (1000 requests, 100 concurrent)
- No dropped connections verification (10000 requests)
- Multiple parallel clients using curl

### Concurrency Tests

- Cache Consistency: 10 threads accessing cache simultaneously
- Queue Full (503): Connection queue saturation test

### Shutdown Tests

- Graceful Shutdown: Server termination under load (Requirement 23)
- No Zombie Processes: Post-shutdown verification (Requirement 24)

### Integrity Tests

- Statistics: Counter accuracy under concurrent load
- Logs: Verification of non-interleaved log entries



## Test File Details

### test_concurrent.c

Tests LRU cache consistency under concurrent access:

- Threads: 10 simultaneous threads
- Iterations: 100 per thread
- Verification: Content integrity in cache

Manual compilation:
```bash
gcc -pthread -o tests/test_cache tests/test_concurrent.c src/cache.c -I src
./tests/test_cache
```

### stress_client.c

Client for connection saturation testing:

- Opens multiple TCP connections simultaneously
- Sends partial requests to keep connections occupied
- Used to test 503 rejection when queue is full

Manual compilation:
```bash
gcc -o tests/stress_client tests/stress_client.c
./tests/stress_client 127.0.0.1 8080 200 5
```

Parameters: `<ip> <port> <num_connections> <duration_seconds>`

---

## Configuration

### test_suite.sh Variables

| Variable | Default Value | Description |
|----------|---------------|-------------|
| `PORT` | 8080 | Server port |
| `WWW_DIR` | www | Web files directory |
| `TEST_TIMEOUT` | 60s | General test timeout |

### Load Parameters

| Mode | Requests | Concurrency | Timeout |
|------|----------|-------------|---------|
| Normal | 1000-10000 | 100 | 120s |
| Helgrind/TSan | 100-500 | 10 | 240s |


## Generated Files

During test execution, the following files are created:

| File | Description |
|------|-------------|
| `server.log` | Server stdout/stderr |
| `access.log` | HTTP access log |
| `valgrind_memcheck.log` | Valgrind output (memcheck mode) |
| `www/` | Directory with test files |

