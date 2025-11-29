#include "thread_pool.h"
#include "stats.h"
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>


void handle_client_request(int client_fd, shared_data_t* shm, semaphores_t* sems){
    long start_time = get_time_ms(); // Get the start time for processing

    char buffer[1024]; // Buffer to hold client request data
    int bytes_sent = 0; // Variable to track bytes sent
    int status_code = 500; // HTTP status code

    // Simple HTTP response for demonstration purposes
    if(read(client_fd, buffer, sizeof(buffer))>0){
        // Send a simple HTTP response
        const char* response = "HTTP/1.1 200 OK\r\nContent-Length: 19\r\n\r\n<h1>Thread Pool</h1>";    
        
        size_t len = strlen(response); // Length of the response

        // Send the response to the client
        if(write(client_fd, response, len) > 0){
            bytes_sent = len; // Update bytes sent
            status_code = 200; // Update status code to 200 OK
        }

        long end_time = get_time_ms(); // Get the end time for processing
        update_stats(shm, sems, status_code, bytes_sent, end_time - start_time); // Update statistics
    }

    close(client_fd); // Close the client connection
}



// Function to create a thread pool
// Arguments:
// num_threads - Number of threads in the pool

thread_pool_t* create_thread_pool(int num_threads, shared_data_t* shm, semaphores_t* sems) {

    thread_pool_t* pool = malloc(sizeof(thread_pool_t)); // Allocate memory for the thread pool structure

    if(!pool) return NULL;

    pool->shm = shm; // Set the shared memory pointer
    pool->sems = sems; // Set the semaphores pointer

    pool->threads = malloc(sizeof(pthread_t) * num_threads); // Allocate memory for the array of threads
    
    pool->num_threads = num_threads; // Set the number of threads in the pool
    pool->shutdown = 0; // Initialize the shutdown flag to 0 (false)
    pool->head = NULL; // Initialize the head pointer to NULL
    pool->tail = NULL; // Initialize the tail pointer to NULL
    pool->job_count = 0; // Initialize the job count to 0

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


// ###################################################################################################################
// FEATURE 2: Thread Pool Management
// ###################################################################################################################

void* worker_thread(void* arg) {
    thread_pool_t* pool = (thread_pool_t*)arg; // Cast the argument to the thread pool structure

    while(1){

        pthread_mutex_lock(&pool->mutex); // Lock the mutex to access the job queue

        // Wait for a job to be available or for shutdown signal
        while(pool -> job_count == 0 && !pool->shutdown){

            // Wait on the condition variable
            // pthread_cond_wait -> Wait for the condition variable to be signaled
            // &pool->cond -> Pointer to the condition variable
            // &pool->mutex -> Pointer to the mutex
            pthread_cond_wait(&pool->cond, &pool->mutex); 
        }

        // Check if the pool is shutting down
        // If shutdown flag is set and no jobs are left, exit the thread
        if(pool->shutdown && pool->job_count == 0){

            // Unlock the mutex before exiting
            // pthread_mutex_unlock -> Unlock the mutex
            // &pool->mutex -> Pointer to the mutex
            pthread_mutex_unlock(&pool->mutex);

            break;
        }

        // Dequeue a job from the queue (FIFO)
        job_t* job = pool->head; // Get the job at the head of the queue

        // Update the head pointer to the next job pool
        // Execute only if there is a job
        if(job){
            pool->head = job->next; // Update the head pointer
            if(pool-> head == NULL ){
                pool -> tail = NULL; // If the queue is empty, update the tail pointer
            }
            pool->job_count--; // Decrement the job count
        }

        pthread_mutex_unlock(&pool->mutex); // Unlock the mutex

        
        // Process the job
        if(job){
            // Process the job
            handle_client_request(job->client_fd, pool->shm, pool->sems); // Handle the client request
            free(job); // Free the job structure
        }
    }
    return NULL; // Exit the thread
}


// Function to Main Thread to add a job to the thread pool
// Arguments:
// pool - Pointer to the thread pool
// client_fd - Client file descriptor to process

void thread_pool_submit (thread_pool_t* pool, int client_fd){
    job_t* job = malloc (sizeof (job_t)); // Allocate memory for the new job

    job->client_fd = client_fd; // Set the client file descriptor
    job->next = NULL; // Initialize the next pointer to NULL

    pthread_mutex_lock(&pool->mutex); // Lock the mutex to access the job queue

    // Add the job to the end of the queue
    // If the queue is empty, set head and tail to the new job
    if(pool-> head == NULL){
        pool->head = job;
        pool->tail = job;
    } else {
        pool->tail->next = job;
        pool->tail = job;
    }

    pool->job_count++; // Increment the job count

    // Unlock the mutex and signal the condition variable
    pthread_cond_signal(&pool->cond);
    pthread_mutex_unlock(&pool->mutex);

}

void destroy_thread_pool(thread_pool_t* pool){

    // Check pool
    if (!pool){
        return;
    }

    // Ask to exit
    pthread_mutex_lock(&pool->mutex); // Lock the mutex to modify the pool state
    pool->shutdown = 1; // Set the shutdown flag to 1
    pthread_cond_broadcast(&pool->cond); // Broadcast the condition variable to wake up all waiting threads
    pthread_mutex_unlock(&pool->mutex); // Unlock the mutex

    for (int i = 0; i < pool->num_threads; i++){
        pthread_join(pool->threads[i], NULL); // Wait for all threads to finish
    }

    // Cleanup
    free(pool->threads); // Free the array of threads
    pthread_mutex_destroy(&pool->mutex); // Destroy the mutex
    pthread_cond_destroy(&pool->cond); // Destroy the condition variable

    // Free remaining jobs in the queue

    job_t* current = pool->head;
    while (current) {
        job_t* next = current->next;
        close(current->client_fd); // Close the client file descriptor
        free(current); // Free the job structure
        current = next;
        }

    free(pool); // Free the thread pool structure
}

