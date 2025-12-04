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
    size_t len = strlen(cfg->document_root);
    if (len > sizeof(g_docroot) - 1) len = sizeof(g_docroot) - 1;
    memcpy(g_docroot, cfg->document_root, len);
    g_docroot[len] = '\0'; // Ensure null-termination

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

// Dequeue a connection from the shared queue
// Returns the connection_item_t if successful, or {-1, -1} on error
static connection_item_t dequeue_connection(shared_data_t* shm, semaphores_t* sems) {
    connection_item_t item = {-1, -1};

    // Wait for an available item
    if (sem_wait(sems->filled_slots) != 0) {
        perror("sem_wait(filled_slots)");
        return item;
    }

    // Critical section for queue access
    if (sem_wait(sems->queue_mutex) != 0) {
        perror("sem_wait(queue_mutex)");
        // Restore the filled slot in case of failure
        sem_post(sems->filled_slots);
        return item;
    }

    // Remove from front
    if (shm->queue.count > 0) {
        item = shm->queue.items[shm->queue.front];
        shm->queue.front = (shm->queue.front + 1) % MAX_QUEUE_SIZE;
        shm->queue.count--;
    }

    // Release critical section
    sem_post(sems->queue_mutex);

    // Signal free slot
    sem_post(sems->empty_slots);

    return item;
}

// Re-enqueue a connection item (used when dequeued item is not for this worker)
static int re_enqueue_connection(shared_data_t* shm, semaphores_t* sems, connection_item_t item) {
    // Wait for a free slot
    if (sem_wait(sems->empty_slots) != 0) {
        perror("sem_wait(empty_slots)");
        return -1;
    }

    // Critical section for queue access
    if (sem_wait(sems->queue_mutex) != 0) {
        perror("sem_wait(queue_mutex)");
        // Restore the free slot in case of failure
        sem_post(sems->empty_slots);
        return -1;
    }

    // Insert at position rear (front + count) % MAX_QUEUE_SIZE
    int pos = (shm->queue.front + shm->queue.count) % MAX_QUEUE_SIZE;
    shm->queue.items[pos] = item;
    shm->queue.count++;

    // Release critical section
    sem_post(sems->queue_mutex);

    // Signal available item
    sem_post(sems->filled_slots);

    return 0;
}

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

    // Create a thread pool with 10 threads and a bounded queue of 2000 jobs
    // Note: the thread pool will typically call the logic that serves requests (HTTP) and,
    //       in that logic, should use the cache via worker_get_cache() and DOCROOT via worker_get_document_root().
    // Max queue size of 2000 prevents memory exhaustion while handling extreme load
    thread_pool_t* pool = create_thread_pool(10, 2000, shm, sems);

    printf("Worker %d: Starting main loop.\n", worker_id);
    fflush(stdout);

    while (worker_running) {
        // First, dequeue a connection item from the shared queue
        connection_item_t item = dequeue_connection(shm, sems);

        // Check if dequeue was successful
        if (item.worker_id == -1) {
            if (!worker_running) break; // shutdown
            continue; // error, try again
        }


        // Check if this connection is for this worker
        if (item.worker_id != worker_id) {
            // Not for this worker, re-enqueue
            if (re_enqueue_connection(shm, sems, item) != 0) {
            }
            continue;
        }


        // This connection is for this worker, now receive the FD via UNIX socket
        int client_fd = recv_fd(channel_fd);

        // Error handling
        if (client_fd < 0) {
            if (!worker_running) break; // shutdown: exit gracefully
            // On error (but not shutdown), continue trying
            continue;
        }


        // Process the client request
        // Submit the client request to the thread pool
        // The handler will:
        //   1) Parse the HTTP request
        //   2) Use the cache via worker_get_cache()
        //   3) Get the document root via worker_get_document_root()
        //   4) Send the response
        //   5) Close the socket when done
        thread_pool_submit(pool, client_fd);
    }

    // Cleanup: destroy the thread pool before exiting
    printf("Worker %d: Shutting down thread pool.\n", worker_id);
    destroy_thread_pool(pool);

    // Destroy worker-specific resources (cache, logger, etc.)
    worker_shutdown_resources();
}
