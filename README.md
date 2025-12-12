# Concurrent HTTP Web Server (SO-TP2)

A multi-threaded HTTP server implemented in C for the Operating Systems course. 
This server uses a Master-Worker architecture with POSIX Shared Memory, Semaphores, and Unix Domain Sockets.

## Features
* **Multi-process & Multi-threaded:** Master process manages fixed-size worker pool.
* **Synchronization:** Uses POSIX named semaphores and mutexes to prevent deadlocks.
* **IPC:** Hybrid approach using Shared Memory (queue/stats) and Unix Domain Sockets (FD passing).
* **Caching:** LRU (Least Recently Used) cache for static files with Reader-Writer locks.
* **Logging:** Thread-safe logging with rotation support.
* **Bonus:** Real-time web dashboard for statistics.

## Quick Start

### Prerequisites
* Linux/Unix environment (POSIX compliant)
* GCC Compiler
* Make

### Compilation
To build the server and auxiliary tools:
```bash
make all
```

### Running the Server
To start the server with default configuration:
```bash
make run
```
The server will start on port 8080 (default).

Access the website: http://localhost:8080

Access the Dashboard (Bonus): http://localhost:8080/dashboard.html

## Testing
To run the automated functional and concurrency test suite:
```bash
make test
```

## Project Structure
- `src/`: Source code files (master, worker, cache, thread_pool, etc.).
- `docs/`: Design document, Report, and User Manual (PDFs).
- `tests/`: Automated test scripts and stress tools.
- `www/`: Static content served by the web server (includes dashboard).
- `server.conf`: Main configuration file.

## Authors
- Martim Gil (124833)
- Nuno Leite Faria (112994)


## References
[1] D. Farolino. (n.d.). domfarolino/4293951bd95082125f2b9931cab1de40 [Gist]. Available: https://gist.github.com/domfarolino/4293951bd95082125f2b9931cab1de40. [Accessed: Dec. 12, 2025].

[2] NGINX. (n.d.). nginx/nginx: The official NGINX Open Source repository. [GitHub Repository]. Available: https://github.com/nginx/nginx. [Accessed: Dec. 12, 2025].
