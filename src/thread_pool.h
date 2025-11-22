#ifndef THREAD_POOL_H
#define THREAD_POOL_H
#include <pthread.h>

// Structure to represent a thread pool
typedef struct {
    pthread_t* threads; // Array of thread identifiers
    int num_threads; // Number of threads in the pool
    pthread_mutex_t mutex; // Mutex for synchronizing access to the pool
    pthread_cond_t cond; // Condition variable for thread synchronization
    int shutdown; // Flag to indicate if the pool is shutting down
} thread_pool_t; // Thread pool structure

// Worker thread function
void* worker_thread(void* arg);

// Function to create a thread pool
thread_pool_t* create_thread_pool(int num_threads);

// Function to destroy a thread pool
void destroy_thread_pool(thread_pool_t* pool);

#endif