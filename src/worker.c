#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <string.h>

#include "worker.h"
#include "shared_mem.h"
#include "semaphores.h"
#include "thread_pool.h"
#include "config.h"
#include "cache.h"     // Cache interface (Feature 4)
#include "logger.h"    // Thread-safe logging (Feature 5)

// ###################################################################################################################
// Sending file descriptors over UNIX sockets
// ###################################################################################################################
// Reference: https://gist.github.com/domfarolino/4293951bd95082125f2b9931cab1de40

/**
 * Receives a file descriptor sent over a Unix domain socket.
 * @param socket The Unix domain socket file descriptor.
 * @return The received file descriptor, or -1 on error.
 */
static int recv_fd(int socket) {
    struct msghdr msg = {0};
    char buf[1];
    struct iovec io = { .iov_base = buf, .iov_len = 1 };

    // Union for control message buffer (ancillary data)
    union {
        char buf[CMSG_SPACE(sizeof(int))];
        struct cmsghdr align;
    } u;

    msg.msg_iov = &io; // I/O vector
    msg.msg_iovlen = 1; // One I/O vector
    msg.msg_control = u.buf; // Control message buffer
    msg.msg_controllen = sizeof(u.buf); // Control message length

    // Receive the message (including ancillary data)
    if (recvmsg(socket, &msg, 0) < 0) {
        // Important: do not exit; return -1 and let the caller decide.
        // EINTR is common during shutdown; the caller will check worker_running.
        perror("Failed to receive fd");
        return -1;
    }

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg); // First control message

    // Validate and extract the file descriptor
    if (cmsg && (cmsg->cmsg_len == CMSG_LEN(sizeof(int)))) {
        if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS) {
            return -1;
        }
        return *((int *)CMSG_DATA(cmsg));
    }

    // No valid ancillary data found
    return -1;
}

// ###################################################################################################################
// GLOBAL STATE (worker process state)
// ###################################################################################################################

// Controls the worker main loop
static volatile int worker_running = 1;

// Per-worker file cache (Feature 4: Thread-Safe File Cache)
static file_cache_t* g_cache = NULL;

// Per-worker document root (copied from config at startup)
static char g_docroot[256];

// ###################################################################################################################
// SIGNAL HANDLER
// ###################################################################################################################

/**
 * Signal handler to gracefully stop the worker process.
 */
void worker_signal_handler(int signum) {
    (void)signum;
    worker_running = 0;
}

// ###################################################################################################################
// FEATURE 4: Cache — initialization, access, and cleanup
// ###################################################################################################################

/**
 * Initializes worker resources that depend on configuration (e.g., cache, document root).
 * cfg -> Pointer to loaded configuration (uses cache_size_mb and num_workers).
 */
void worker_init_resources(const server_config_t* cfg) {
    // Copy DOCUMENT_ROOT to local worker memory (null-terminated string)
    strncpy(g_docroot, cfg->document_root, sizeof(g_docroot) - 1);
    g_docroot[sizeof(g_docroot) - 1] = '\0'; // Ensure null-termination

    // Initialize thread-safe/process-safe logger (Feature 5)
    // (each worker reopens the same log file with global semaphore)
    logger_init(cfg->log_file);

    // Total desired capacity in bytes (config gives in megabytes)
    size_t cap = (size_t)cfg->cache_size_mb * 1024ULL * 1024ULL;

    // Optional: divide total capacity by number of workers,
    // to avoid each worker trying to use the full memory.
    if (cfg->num_workers > 0) {
        cap /= (size_t)cfg->num_workers;
        // Impose a reasonable minimum to avoid too small caches.
        if (cap < (1u << 20)) cap = (1u << 20);  // minimum 1 MiB
    }

    // Create the thread-safe LRU cache
    g_cache = cache_create(cap);

    if (!g_cache) {
        fprintf(stderr, "WORKER: Failed to create cache (cap=%zu bytes).\n", cap);
        exit(1);
    }

    // Informational log for debugging
    fprintf(stderr, "Worker: Cache initialized with %zu bytes. DOCROOT=%s\n", cap, g_docroot);
}

/**
 * Returns the worker's cache pointer for use by other modules.
 */
file_cache_t* worker_get_cache(void) {
    return g_cache;
}

/**
 * Returns the worker's document root (internal string; do not modify).
 */
const char* worker_get_document_root(void) {
    return g_docroot;
}

/**
 * Cleans up and destroys worker-specific resources (cache, logger, etc.).
 */
void worker_shutdown_resources(void) {
    if (g_cache) {
        cache_destroy(g_cache);
        g_cache = NULL;
    }
    logger_close();
}

// ###################################################################################################################
// FEATURE 1: Connection Queue Consumer
// ###################################################################################################################

/**
 * Worker main function.
 * Waits for connections in the shared memory queue, receives the client socket
 * from the master via UNIX socket, and submits it to the thread pool for processing.
 *
 * shm -> Shared memory pointer.
 * sems -> Pointer to semaphores structure.
 * worker_id -> Worker process identifier.
 * channel_fd -> UNIX socket file descriptor for receiving client sockets.
 */
void worker_main(shared_data_t* shm, semaphores_t* sems, int worker_id, int channel_fd) {
    // Register signal handler for graceful shutdown
    signal(SIGTERM, worker_signal_handler);
    signal(SIGINT, worker_signal_handler);

    // Create a thread pool with 10 threads
    // Note: the thread pool will typically call the logic that serves requests (HTTP) and,
    //       in that logic, should use the cache via worker_get_cache() and DOCROOT via worker_get_document_root().
    thread_pool_t* pool = create_thread_pool(10, shm, sems);

    printf("Worker %d: Starting main loop.\n", worker_id);

    while (worker_running) {
        // Wait for an available connection in the queue
        // Handle EINTR so that Ctrl+C/SIGTERM can wake up the worker
        while (worker_running) {
            if (sem_wait(sems->filled_slots) == 0) break;
            if (errno == EINTR) {
                if (!worker_running) break;  // exit on shutdown
                continue;  // transient signal — retry wait
            }
            perror("Worker sem_wait(filled_slots) error");
            // non-EINTR error: try to continue the main loop
            break;
        }
        if (!worker_running) break;

        // Exclusive access to queue
        while (worker_running) {
            if (sem_wait(sems->queue_mutex) == 0) break;
            if (errno == EINTR) {
                if (!worker_running) break;
                continue;
            }
            perror("Worker sem_wait(queue_mutex) error");
            // Failed to acquire mutex: return filled slot to not lose signal
            sem_post(sems->filled_slots);
            // and proceed to next cycle
            goto next_iteration;
        }
        if (!worker_running) {
            // if exiting due to shutdown, do not leave mutex locked
            // (only entered while if lock was acquired)
            // so nothing to do
            break;
        }

        // Dequeue client socket (not used here, as we receive via UNIX socket)
        int ignore_fd = shm->queue.sockets[shm->queue.front];
        (void)ignore_fd;

        // Update queue front and count
        shm->queue.front = (shm->queue.front + 1) % MAX_QUEUE_SIZE;
        shm->queue.count--;

        // Unlock the queue and signal that a slot is now free
        sem_post(sems->queue_mutex);
        sem_post(sems->empty_slots);

        // Receive the client file descriptor from the master via UNIX socket
        // channel_fd -> Channel file descriptor
        // recv_fd -> Function to receive file descriptor
        // client_fd -> Client socket file descriptor
        {
            int client_fd = recv_fd(channel_fd);

            // Error handling
            if (client_fd < 0) {
                if (!worker_running) break; // shutdown: sair
                fprintf(stderr, "Worker %d: Falha ao receber descritor real.\n", worker_id);
                goto next_iteration;
            }

            // Process the client request
            printf("Worker %d: Processing client socket %d\n", worker_id, client_fd);
            
            // Submit the client request to the thread pool
            // thread_pool_submit -> entrega o descritor para processamento assíncrono
            // O handler associado ao pool deve:
            //   1) Interpretar o pedido HTTP
            //   2) Consultar/usar a cache com worker_get_cache()
            //   3) Obter o document root com worker_get_document_root()
            //   4) Escrever a resposta no socket
            //   5) Fechar o socket no fim do processamento
            thread_pool_submit(pool, client_fd);
        }

    next_iteration:
        ; // no-op label target
    }

    // Cleanup: destroy the thread pool before exiting
    printf("Worker %d: Shutting down thread pool.\n", worker_id);
    destroy_thread_pool(pool);

    // Destroy worker-specific resources (cache, logger, etc.)
    worker_shutdown_resources();
}
