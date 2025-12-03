#ifndef WORKER_H
#define WORKER_H

#include "config.h"     // server_config_t
#include "cache.h"      // file_cache_t (per-worker cache)
#include "shared_mem.h" // Shared memory structures (connection queue, stats)
#include "semaphores.h" // Semaphores for queue synchronization

// ###################################################################################################################
// Worker Lifecycle API
// ###################################################################################################################

// Initializes worker-specific resources. Called in the child process after master forks.
void worker_init_resources(const server_config_t* cfg);

// Releases worker-specific resources (e.g., cache).
void worker_shutdown_resources(void);

// Returns a pointer to the worker's thread-safe LRU cache.
file_cache_t* worker_get_cache(void);

// Returns the worker's document root path.
const char* worker_get_document_root(void);

// ###################################################################################################################
// Worker Main Loop
// ###################################################################################################################

// Main function for a worker process.
// shm        -> Pointer to shared memory (connection queue, stats).
// sems       -> Semaphores for queue synchronization.
// worker_id  -> Logical ID of this worker.
// channel_fd -> UNIX socket for receiving client file descriptors from master.
void worker_main(shared_data_t* shm, semaphores_t* sems, int worker_id, int channel_fd);

#endif /* WORKER_H */
