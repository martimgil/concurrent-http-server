#include "thread_pool.h"
#include "stats.h"
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <strings.h>   // strcasecmp
#include <stdio.h>
#include <semaphore.h> // For log_request function
#include "worker.h"    // worker_get_cache(), worker_get_document_root()
#include "cache.h"     // file_cache_t (Feature 4: cache per worker)
#include "logger.h"    // logger_write (Feature 5: thread-safe/process-safe logging)

// ----------------------------------------------------------------------------------------
// Forward declarations (http_parser.h / http_builder.h not included, declared here)
// ----------------------------------------------------------------------------------------

// Minimal structure used by HTTP parser (compatible with http_parser.c)
typedef struct {
    char method[8];
    char path[512];
    char version[16];
} http_request_t;

// parse_http_request -> Implemented in http_parser.c
int parse_http_request(const char* raw, http_request_t* out_req);

// send_http_response -> Implemented in http_builder.c
void send_http_response(int fd, int status, const char* status_msg,
                        const char* content_type, const char* body, size_t body_len);

// log_request -> Implemented in thread_logger.c
// Logs HTTP requests in a thread-safe manner
void log_request(sem_t* log_sem, const char* client_ip, const char* method, 
                 const char* path, int status, size_t bytes);

// ----------------------------------------------------------------------------------------
// ----------------------------------------------------------------------------------------
// Helper: Determine MIME type based on file extension for HTTP responses
// ----------------------------------------------------------------------------------------
// Returns a string representing the MIME type for a given file path.
// - path: The requested resource path (e.g., "/index.html").
// - Looks for the file extension after the last '.' character.
// - Compares extension (case-insensitive) to known types.
// - Returns a default MIME type if extension is missing or unknown.
static const char* mime_type_from_path(const char* path) {
    // Find the last occurrence of '.' in the path (file extension)
    const char* ext = strrchr(path, '.');
    if (!ext)
        return "application/octet-stream"; // No extension: use generic binary type

    ext++; // Move pointer past the '.'

    // Common web file types (case-insensitive comparison)
    if (!strcasecmp(ext, "html"))
        return "text/html";
    if (!strcasecmp(ext, "htm"))
        return "text/html";
    if (!strcasecmp(ext, "css"))
        return "text/css";
    if (!strcasecmp(ext, "js"))
        return "application/javascript";
    if (!strcasecmp(ext, "png"))
        return "image/png";
    if (!strcasecmp(ext, "jpg"))
        return "image/jpeg";
    if (!strcasecmp(ext, "jpeg"))
        return "image/jpeg";
    if (!strcasecmp(ext, "gif"))
        return "image/gif";
    if (!strcasecmp(ext, "svg"))
        return "image/svg+xml";
    if (!strcasecmp(ext, "json"))
        return "application/json";

    // Unknown or unhandled extension: use generic binary type
    return "application/octet-stream";
}


// ###################################################################################################################
// FEATURE 2 + 4 + 5: HTTP Handler with LRU Cache and Logging
// ###################################################################################################################

void handle_client_request(int client_fd, shared_data_t* shm, semaphores_t* sems){
    long start_time = get_time_ms(); // Get the start time for processing

    char buffer[8192]; // Buffer to hold client request data
    int bytes_sent = 0; // Variable to track bytes sent
    int status_code = 500; // HTTP status code (default: 500)

    // Read HTTP request from client
    // read -> Reads bytes from client socket into buffer
    ssize_t n = read(client_fd, buffer, sizeof(buffer)-1);

    // Check for errors or closed connection
    if (n <= 0){
        close(client_fd); // Close the client connection
        return;
    }

    buffer[n] = '\0'; // Null-terminate to operate as string

    // ----------------------------------------------------------------------------------------
    // HTTP REQUEST PARSING (http_parser.c)
    // ----------------------------------------------------------------------------------------
    http_request_t req;

    // parse_http_request -> Extract method, path and version from first line
    if (parse_http_request(buffer, &req) != 0){
        // Malformed request → 400 Bad Request
        const char* msg = "<h1>400 Bad Request</h1>";
        send_http_response(client_fd, 400, "Bad Request", "text/html", msg, strlen(msg));
        status_code = 400;
        bytes_sent = (int)strlen(msg);

        long end_time = get_time_ms(); // Get the end time for processing
        update_stats(shm, sems, status_code, bytes_sent, end_time - start_time); // Update statistics

        // Logging (Feature 5) - Log malformed HTTP request
        logger_write("127.0.0.1", "?", "?", status_code, (size_t)bytes_sent, end_time - start_time);
        log_request(sems->log_mutex, "127.0.0.1", "?", "?", status_code, (size_t)bytes_sent);

        close(client_fd); // Close the client connection
        return;
    }

    // ----------------------------------------------------------------------------------------
    // ONLY GET METHOD IS SUPPORTED AT THIS STAGE
    // ----------------------------------------------------------------------------------------
    if (strcmp(req.method, "GET") != 0){
        const char* msg = "<h1>405 Method Not Allowed</h1>";
        send_http_response(client_fd, 405, "Method Not Allowed", "text/html", msg, strlen(msg));
        status_code = 405;
        bytes_sent = (int)strlen(msg);

        long end_time = get_time_ms(); // Get the end time for processing
        update_stats(shm, sems, status_code, bytes_sent, end_time - start_time); // Update statistics

        // Logging (Feature 5) - Log unsupported HTTP method
        logger_write("127.0.0.1", req.method, req.path, status_code, (size_t)bytes_sent, end_time - start_time);
        log_request(sems->log_mutex, "127.0.0.1", req.method, req.path, status_code, (size_t)bytes_sent);

        close(client_fd); // Close the client connection
        return;
    }

    // ========================================================================================
    // BUILD ABSOLUTE FILE PATH (document_root + requested_path)
    // ========================================================================================

    // Retrieve the document root directory configured for this worker process
    const char* docroot = worker_get_document_root();

    // Map requested path to actual resource path
    // Serve index.html as default for root directory requests
    const char* relpath = (strcmp(req.path, "/") == 0) ? "/index.html" : req.path;

    // Construct full absolute path by concatenating document root and relative path
    // This path is used to locate files on the file system
    char abs_path[1024];
    snprintf(abs_path, sizeof(abs_path), "%s%s", docroot, relpath);

    // ========================================================================================
    // LRU CACHE LOOKUP (Feature 4): Try cache HIT first; on MISS, load and insert
    // ========================================================================================
    // This cache layer improves performance by storing recently accessed files in memory.
    // Each worker process maintains its own LRU cache to minimize lock contention.

    // Retrieve the per-worker LRU cache instance for this worker process
    file_cache_t* cache = worker_get_cache();

    // Handle for accessing cached data with automatic reference counting
    // Maintains reference count to prevent cache items from being evicted while in use
    cache_handle_t h;

    // 1) Try cache HIT
    if (cache_acquire(cache, relpath, &h)){
        // Determine MIME type of resource
        const char* content_type = mime_type_from_path(relpath);

        // Send HTTP response from cached content
        send_http_response(client_fd, 200, "OK", content_type, (const char*)h.data, h.size);

        status_code = 200; // Update status code
        bytes_sent = (int)h.size; // Update bytes sent

        // Release reference to cached item (decrements refcount and allows eviction if unused)
        cache_release(cache, &h);

        // Record end time and update statistics
        long end_time = get_time_ms(); // Get the end time for processing
        update_stats(shm, sems, status_code, bytes_sent, end_time - start_time); // Update statistics

        // Logging (Feature 5) - calls log_request for detailed HTTP request logging (cache hit)
        logger_write("127.0.0.1", req.method, req.path, status_code, (size_t)bytes_sent, end_time - start_time);
        log_request(sems->log_mutex, "127.0.0.1", req.method, req.path, status_code, (size_t)bytes_sent);

        close(client_fd); // Close the client connection
        return;
    }

    // 2) MISS: Check if file exists on the file system
    if (access(abs_path, F_OK) != 0){
        // File does not exist → 404 Not Found response
        const char* msg = "<h1>404 Not Found</h1>";
        send_http_response(client_fd, 404, "Not Found", "text/html", msg, strlen(msg));
        status_code = 404;
        bytes_sent = (int)strlen(msg);

        long end_time = get_time_ms(); // Get the end time for processing
        update_stats(shm, sems, status_code, bytes_sent, end_time - start_time); // Update statistics

        // Logging (Feature 5) - Log failed file access attempt
        logger_write("127.0.0.1", req.method, req.path, status_code, (size_t)bytes_sent, end_time - start_time);
        log_request(sems->log_mutex, "127.0.0.1", req.method, req.path, status_code, (size_t)bytes_sent);

        close(client_fd); // Close the client connection
        return;
    }

    // 3) MISS with existing file: Load from disk and insert into cache
    if (!cache_load_file(cache, relpath, abs_path, &h)){
        // File read failed → 500 Internal Server Error response
        const char* msg = "<h1>500 Internal Server Error</h1>";
        send_http_response(client_fd, 500, "Internal Server Error", "text/html", msg, strlen(msg));
        status_code = 500;
        bytes_sent = (int)strlen(msg);

        // Record end time and update server statistics
        long end_time = get_time_ms(); // Get the end time for processing
        update_stats(shm, sems, status_code, bytes_sent, end_time - start_time); // Update statistics

        // Logging (Feature 5) - Log file I/O error during cache load
        logger_write("127.0.0.1", req.method, req.path, status_code, (size_t)bytes_sent, end_time - start_time);
        log_request(sems->log_mutex, "127.0.0.1", req.method, req.path, status_code, (size_t)bytes_sent);

        close(client_fd); // Close the client connection
        return;
    }

    // 4) After loading: Send response with content now in cache (cache MISS + successful load)
    {
        // Determine MIME type based on file extension for correct content-type header
        const char* content_type = mime_type_from_path(relpath);

        // Send successful HTTP response with file content from cache
        send_http_response(client_fd, 200, "OK", content_type, (const char*)h.data, h.size);

        status_code = 200; // Update status code to success
        bytes_sent = (int)h.size; // Update bytes sent to file size

        // Release reference to cached item (decrements refcount and allows eviction if unused)
        cache_release(cache, &h);

        // Record end time and update server statistics
        long end_time = get_time_ms(); // Get the end time for processing
        update_stats(shm, sems, status_code, bytes_sent, end_time - start_time); // Update statistics

        // Logging (Feature 5) - Log successful request (cache miss but file loaded successfully)
        logger_write("127.0.0.1", req.method, req.path, status_code, (size_t)bytes_sent, end_time - start_time);
        log_request(sems->log_mutex, "127.0.0.1", req.method, req.path, status_code, (size_t)bytes_sent);
    }

    // Close the client socket connection
    close(client_fd); // Close the client connection
}



// Function to create a thread pool with worker threads
// Arguments:
// num_threads - Number of worker threads to create
// shm - Pointer to shared memory data for inter-process communication
// sems - Pointer to semaphores for synchronization
// Returns: Pointer to newly created thread pool, or NULL on failure

thread_pool_t* create_thread_pool(int num_threads, shared_data_t* shm, semaphores_t* sems) {

    // Allocate memory for the thread pool structure
    thread_pool_t* pool = malloc(sizeof(thread_pool_t));

    // Check if memory allocation failed
    if(!pool) return NULL;

    // Initialize shared memory and semaphores pointers for worker threads to use
    pool->shm = shm;
    pool->sems = sems;

    // Allocate memory for array of thread identifiers
    pool->threads = malloc(sizeof(pthread_t) * num_threads);
    
    // Initialize pool state variables
    pool->num_threads = num_threads;        // Store number of threads
    pool->shutdown = 0;                     // Initialize shutdown flag (0 = false, still running)
    pool->head = NULL;                      // Initialize job queue head (empty)
    pool->tail = NULL;                      // Initialize job queue tail (empty)
    pool->job_count = 0;                    // Initialize job counter (no jobs)

    // Initialize the mutex for protecting shared pool state
    // The mutex ensures only one thread can access pool->job_count, head, tail at a time
    pthread_mutex_init(&pool->mutex, NULL);
    
    // Initialize the condition variable for job queue notifications
    // Worker threads will wait on this when queue is empty
    pthread_cond_init(&pool->cond, NULL);

    // Create worker threads that will wait for and process incoming jobs
    for (int i = 0; i < num_threads; i++) {
        // Create new thread that executes worker_thread function
        // Pass the pool pointer so threads can access job queue and state
        pthread_create(&pool->threads[i], NULL, worker_thread, pool);
    }

    // Return pointer to initialized thread pool
    return pool;
}


// ###################################################################################################################
// FEATURE 2: Thread Pool Management - Worker Thread Function
// ###################################################################################################################
// Worker thread main loop: continuously waits for and processes client requests
// Each thread dequeues jobs (client connections) and processes them serially

void* worker_thread(void* arg) {
    // Cast and extract thread pool pointer from generic argument
    thread_pool_t* pool = (thread_pool_t*)arg;

    // Main worker loop - runs until pool is shutdown and all jobs are processed
    while(1){
        // Acquire lock to safely access shared pool state and job queue
        pthread_mutex_lock(&pool->mutex);

        // Wait for a job to be available or shutdown signal
        // Loop continues while queue is empty AND pool is still running
        while(pool->job_count == 0 && !pool->shutdown){
            // Release lock and wait for condition signal (job added or shutdown)
            // When signaled, reacquire lock and resume
            pthread_cond_wait(&pool->cond, &pool->mutex); 
        }

        // Check if shutdown is requested with no jobs remaining
        // This allows graceful exit after processing all pending jobs
        if(pool->shutdown && pool->job_count == 0){
            // Release lock before exiting thread
            pthread_mutex_unlock(&pool->mutex);
            // Exit worker thread
            break;
        }

        // Dequeue a job from the front of the FIFO queue
        job_t* job = pool->head;

        // If there is a job, remove it from queue and update counters
        if(job){
            // Move head pointer to next job in queue
            pool->head = job->next;
            
            // If queue becomes empty after dequeue, clear tail pointer
            if(pool->head == NULL){
                pool->tail = NULL;
            }
            
            // Decrement remaining job count
            pool->job_count--;
        }

        // Release lock to allow other threads to access queue
        pthread_mutex_unlock(&pool->mutex);

        // Process the dequeued job outside of lock (important for concurrency)
        if(job){
            // Handle the HTTP client request (includes parsing, caching, and response)
            handle_client_request(job->client_fd, pool->shm, pool->sems);
            
            // Free the job structure after processing
            free(job);
        }
    }
    
    // Exit thread (implicit return)
    return NULL;
}


// Main thread function to enqueue a client connection to the thread pool
// Submits client file descriptor as a new job for worker threads to process
// Arguments:
// pool - Pointer to the thread pool structure
// client_fd - File descriptor of accepted client socket connection

void thread_pool_submit (thread_pool_t* pool, int client_fd){
    // Allocate memory for new job structure
    job_t* job = malloc (sizeof (job_t));

    // Initialize job with client file descriptor
    job->client_fd = client_fd;     // Store client socket descriptor
    job->next = NULL;               // Clear next pointer (will be set when enqueued)

    // Acquire lock before modifying shared job queue
    pthread_mutex_lock(&pool->mutex);

    // Enqueue job at end of FIFO queue
    // If queue is empty, this is both head and tail
    if(pool->head == NULL){
        pool->head = job;       // First job in queue
        pool->tail = job;
    } else {
        // Append to existing queue by linking through tail
        pool->tail->next = job;
        pool->tail = job;       // Update tail to new job
    }

    // Increment counter of pending jobs
    pool->job_count++;

    // Signal condition variable to wake up a waiting worker thread
    // Unlock must follow signal to avoid race conditions
    pthread_cond_signal(&pool->cond);
    pthread_mutex_unlock(&pool->mutex);

}

// Gracefully shutdown thread pool, wait for all threads to complete, and free all resources
// This function ensures all pending jobs are finished before cleanup
void destroy_thread_pool(thread_pool_t* pool){

    // Verify pool pointer is valid
    if (!pool){
        return;
    }

    // Signal all worker threads to shutdown after processing current jobs
    pthread_mutex_lock(&pool->mutex);
    pool->shutdown = 1;                      // Set shutdown flag (signals threads to exit)
    pthread_cond_broadcast(&pool->cond);     // Wake up all worker threads to check shutdown flag
    pthread_mutex_unlock(&pool->mutex);

    // Wait for all worker threads to finish their execution and exit
    for (int i = 0; i < pool->num_threads; i++){
        pthread_join(pool->threads[i], NULL); // Block until thread i completes
    }

    // Cleanup: Free synchronization primitives and thread array
    free(pool->threads);                    // Free thread ID array
    pthread_mutex_destroy(&pool->mutex);    // Destroy mutex
    pthread_cond_destroy(&pool->cond);      // Destroy condition variable

    // Process any remaining jobs in queue that may not have been processed
    job_t* current = pool->head;
    while (current) {
        job_t* next = current->next;        // Save next job before freeing current
        close(current->client_fd);          // Close associated client socket
        free(current);                      // Free job structure
        current = next;
    }

    // Free the thread pool structure itself
    free(pool);
}
