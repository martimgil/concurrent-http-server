#include "thread_pool.h"
#include <stdlib.h>

// Function to create a thread pool
// Arguments:
// num_threads - Number of threads in the pool

thread_pool_t* create_thread_pool(int num_threads) {

    thread_pool_t* pool = malloc(sizeof(thread_pool_t)); // Allocate memory for the thread pool structure

    pool->threads = malloc(sizeof(pthread_t) * num_threads); // Allocate memory for the array of threads
    
    pool->num_threads = num_threads; // Set the number of threads in the pool
    pool->shutdown = 0; // Initialize the shutdown flag to 0 (false)

    // Initialize the mutex
    // pthread_mutex_init -> Initialize the mutex
    // &pool->mutex -> Pointer to the mutex
    // NULL -> Use default attributes
    pthread_mutex_init(&pool->mutex, NULL);
    
    // Initialize the condition variable
    // pthread_cond_init -> Initialize the condition variable
    // &pool->cond -> Pointer to the condition variable
    // NULL -> Use default attributes
    pthread_cond_init(&pool->cond, NULL);


    // Create worker threads
    // num_threads -> Number of threads to create
    for (int i = 0; i < num_threads; i++) {

        // pthread_create -> Create a new thread
        // &pool->threads[i] -> Pointer to the thread identifier
        // NULL -> Use default thread attributes
        // worker_thread -> Function to be executed by the thread
        // pool -> Argument to be passed to the thread function

        pthread_create(&pool->threads[i], NULL, worker_thread, pool);
    }

    return pool; // Return the created thread pool
}