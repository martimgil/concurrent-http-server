#ifndef THREAD_POOL_H
#define THREAD_POOL_H
#include <pthread.h>
#include "shared_mem.h"
#include "semaphores.h"

// ###################################################################################################################
// FEATURE 2: Thread Pool Management
// ###################################################################################################################

// Structure to represent a job in the thread pool
typedef struct job {
    int client_fd; // Client file descriptor to process
    struct job* next; // Pointer to the next job in the queue
} job_t;

// Structure to represent a thread pool
typedef struct {
    pthread_t* threads; // Array of thread identifiers
    int num_threads; // Number of threads in the pool

    pthread_mutex_t mutex; // Mutex for synchronizing access to the pool
    pthread_cond_t cond; // Condition variable for thread synchronization
    int shutdown; // Flag to indicate if the pool is shutting down

    job_t* head; // Pointer to the head of the job queue
    job_t* tail; // Pointer to the tail of the job queue
    int job_count; // Number of jobs in the queue

    shared_data_t* shm; // Pointer to shared memory for statistics
    semaphores_t* sems; // Pointer to semaphores for synchronization
} thread_pool_t;

thread_pool_t* create_thread_pool(int num_threads, shared_data_t* shm, semaphores_t* sems); // Create a new thread pool

void destroy_thread_pool(thread_pool_t* pool); // Destroy the thread pool   
void thread_pool_submit(thread_pool_t* pool, int client_fd); // Submit a job to the thread pool
void handle_client_request(int client_fd, shared_data_t* shm, semaphores_t* sems); // Handle client request


void add_job(thread_pool_t* pool, int client_fd); // Add a job to the thread pool

// Worker thread function
void* worker_thread(void* arg);

#endif