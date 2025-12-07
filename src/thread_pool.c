#include "thread_pool.h"
#include "stats.h"
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <strings.h>   // strcasecmp
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>
#include <sys/socket.h>
#include <errno.h>     // Required for EWOULDBLOCK/EAGAIN
#include "worker.h"    // worker_get_cache(), worker_get_document_root()
#include "cache.h"     // file_cache_t (Feature 4: cache per worker)
#include "logger.h"    // logger_write (Feature 5: thread-safe/process-safe logging)
#include "http_parser.h" // Shared definition of http_request_t
#include "http_builder.h" // send_http_partial_response

// ----------------------------------------------------------------------------------------
// Forward declarations
// ----------------------------------------------------------------------------------------

// parse_http_request -> Implemented in http_parser.c
// int parse_http_request(const char* raw, http_request_t* out_req); // Now in header

// send_http_response -> Implemented in http_builder.c
// void send_http_response(...) // Now in header

// ----------------------------------------------------------------------------------------
// ----------------------------------------------------------------------------------------
// Helper: Determine MIME type based on file extension for HTTP responses
// ----------------------------------------------------------------------------------------
static const char* mime_type_from_path(const char* path) {
    const char* ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";
    ext++;
    if (!strcasecmp(ext, "html")) return "text/html";
    if (!strcasecmp(ext, "htm")) return "text/html";
    if (!strcasecmp(ext, "css")) return "text/css";
    if (!strcasecmp(ext, "js")) return "application/javascript";
    if (!strcasecmp(ext, "png")) return "image/png";
    if (!strcasecmp(ext, "jpg")) return "image/jpeg";
    if (!strcasecmp(ext, "jpeg")) return "image/jpeg";
    if (!strcasecmp(ext, "gif")) return "image/gif";
    if (!strcasecmp(ext, "svg")) return "image/svg+xml";
    if (!strcasecmp(ext, "json")) return "application/json";
    return "application/octet-stream";
}

// Helper: Validate path to prevent directory traversal attacks
static int is_path_safe(const char* path) {
    if (strstr(path, "..") != NULL) return 0;
    return 1;
}

// Helper: Handle sending content (full or partial)
static void send_content(int client_fd, const char* content_type, const char* data, size_t total_size, 
                         http_request_t* req, int keep_alive, int is_head_request, 
                         int* status_code, int* bytes_sent) {
    
    long start = 0;
    long end = total_size - 1;
    int is_partial = 0;

    // Parse Range header if present
    if (req->range[0] != '\0') {
        if (strncasecmp(req->range, "bytes=", 6) == 0) {
            char* range_val = req->range + 6;
            char* dash = strchr(range_val, '-');
            if (dash) {
                *dash = '\0';
                const char* start_str = range_val;
                const char* end_str = dash + 1;
                
                long req_start = -1, req_end = -1;
                if (*start_str) req_start = atol(start_str);
                if (*end_str) req_end = atol(end_str);
                
                // Restore dash for logging/debugging if needed
                *dash = '-';

                if (req_start != -1 && req_end != -1) {
                    start = req_start;
                    end = req_end;
                } else if (req_start != -1) {
                    start = req_start;
                    end = total_size - 1;
                } else if (req_end != -1) {
                    start = total_size - req_end;
                    end = total_size - 1;
                }
                is_partial = 1;
            }
        }
    }

    // Validate range
    if (is_partial && (start < 0 || end >= (long)total_size || start > end)) {
        send_error_response(client_fd, 416, "Range Not Satisfiable", keep_alive);
        *status_code = 416;
        *bytes_sent = 0;
        return;
    }

    if (is_partial) {
        size_t chunk_len = end - start + 1;
        if (!is_head_request) {
            send_http_partial_response(client_fd, content_type, data + start, chunk_len, start, end, total_size, keep_alive);
        } else {
            // For HEAD partial, we still send 206 headers but no body
            send_http_partial_response(client_fd, content_type, NULL, chunk_len, start, end, total_size, keep_alive);
        }
        *status_code = 206;
        *bytes_sent = chunk_len;
    } else {
        send_http_response_with_body_flag(client_fd, 200, "OK", content_type, data, total_size, !is_head_request, keep_alive);
        *status_code = 200;
        *bytes_sent = total_size;
    }
}

// ###################################################################################################################
// FEATURE 2 + 4 + 5 + Keep-Alive: HTTP Handler with LRU Cache and Logging
// ###################################################################################################################

void handle_client_request(int client_fd, shared_data_t* shm, semaphores_t* sems){
    
    long start_time = get_time_ms(); 

    char buffer[8192]; 
    int bytes_sent = 0; 
    int status_code = 500; 

    ssize_t total_bytes = 0;
    ssize_t n;
    
    while (total_bytes < (ssize_t)(sizeof(buffer) - 1)) {
        n = read(client_fd, buffer + total_bytes, sizeof(buffer) - 1 - total_bytes);
        
        if (n < 0) {
            close(client_fd);
            return;
        }
        
        if (n == 0) {
            close(client_fd);
            return;
        }
        
        total_bytes += n;
        
        if (total_bytes >= 4 && 
            buffer[total_bytes-4] == '\r' && buffer[total_bytes-3] == '\n' &&
            buffer[total_bytes-2] == '\r' && buffer[total_bytes-1] == '\n') {
            break; 
        }
    }

    if (total_bytes == 0) {
        close(client_fd);
        return;
    }

    buffer[total_bytes] = '\0'; 

    http_request_t req;

    if (parse_http_request(buffer, &req) != 0){
        send_error_response(client_fd, 400, "Bad Request", 0);
        status_code = 400;
        bytes_sent = 0;
        long end_time = get_time_ms(); 
        update_stats(shm, sems, status_code, bytes_sent, end_time - start_time); 
        logger_write("127.0.0.1", "?", "?", status_code, (size_t)bytes_sent, end_time - start_time);
        close(client_fd);
        return;
    }

    int is_head_request = (strcmp(req.method, "HEAD") == 0);
    if (strcmp(req.method, "GET") != 0 && !is_head_request){
        send_error_response(client_fd, 405, "Method Not Allowed", 0);
        status_code = 405;
        bytes_sent = 0;
        long end_time = get_time_ms(); 
        update_stats(shm, sems, status_code, bytes_sent, end_time - start_time); 
        logger_write("127.0.0.1", req.method, req.path, status_code, (size_t)bytes_sent, end_time - start_time);
        close(client_fd);
        return;
    }

    // BONUS FEATURE: Real-time Dashboard API endpoint
    if (strcmp(req.path, "/api/stats") == 0) {
        // Read stats from shared memory (thread-safe)
        sem_wait(sems->stats_mutex);
        long total_reqs = shm->stats.total_requests;
        long bytes_trans = shm->stats.bytes_transferred;
        long s200 = shm->stats.status_200;
        long s404 = shm->stats.status_404;
        long s500 = shm->stats.status_500;
        long total_time = shm->stats.total_response_time_ms;
        int active = shm->stats.active_connections;
        sem_post(sems->stats_mutex);
        
        // Calculate average response time
        double avg_time = (total_reqs > 0) ? (double)total_time / total_reqs : 0.0;
        
        // Get cache stats
        file_cache_t* cache = worker_get_cache();
        size_t cache_items = 0, cache_bytes = 0, cache_capacity = 0;
        size_t cache_hits = 0, cache_misses = 0, cache_evictions = 0;
        if (cache) {
            cache_stats(cache, &cache_items, &cache_bytes, &cache_capacity, 
                       &cache_hits, &cache_misses, &cache_evictions);
        }
        
        // Build JSON response
        char json[2048];
        int json_len = snprintf(json, sizeof(json),
            "{"
            "\"total_requests\":%ld,"
            "\"bytes_transferred\":%ld,"
            "\"active_connections\":%d,"
            "\"avg_response_time_ms\":%.2f,"
            "\"status_codes\":{"
                "\"200\":%ld,"
                "\"404\":%ld,"
                "\"500\":%ld"
            "},"
            "\"cache\":{"
                "\"items\":%zu,"
                "\"bytes_used\":%zu,"
                "\"capacity\":%zu,"
                "\"hits\":%zu,"
                "\"misses\":%zu,"
                "\"evictions\":%zu,"
                "\"hit_rate\":%.2f"
            "},"
            "\"uptime_info\":\"Running\""
            "}",
            total_reqs, bytes_trans, active, avg_time,
            s200, s404, s500,
            cache_items, cache_bytes, cache_capacity,
            cache_hits, cache_misses, cache_evictions,
            (cache_hits + cache_misses > 0) ? 
                (double)cache_hits / (cache_hits + cache_misses) * 100.0 : 0.0
        );
        
        send_http_response(client_fd, 200, "OK", "application/json", json, json_len, 0);
        status_code = 200;
        bytes_sent = json_len;
        long end_time = get_time_ms();
        update_stats(shm, sems, status_code, bytes_sent, end_time - start_time);
        logger_write("127.0.0.1", req.method, req.path, status_code, (size_t)bytes_sent, end_time - start_time);
        close(client_fd);
        return;
    }

    const char* docroot = worker_get_document_root();
    const char* relpath = (strcmp(req.path, "/") == 0) ? "/index.html" : req.path;

    if (!is_path_safe(relpath)) {
        send_error_response(client_fd, 403, "Forbidden", 0);
        status_code = 403;
        bytes_sent = 0;
        long end_time = get_time_ms();
        update_stats(shm, sems, status_code, bytes_sent, end_time - start_time);
        logger_write("127.0.0.1", req.method, req.path, status_code, (size_t)bytes_sent, end_time - start_time);
        close(client_fd);
        return;
    }

    char abs_path[1024];
    snprintf(abs_path, sizeof(abs_path), "%s%s", docroot, relpath);

    file_cache_t* cache = worker_get_cache();
    cache_handle_t h;

    if (cache_acquire(cache, relpath, &h)){
        const char* content_type = mime_type_from_path(relpath);
        
        // Use helper to handle range/full content
        send_content(client_fd, content_type, (const char*)h.data, h.size, &req, 0, is_head_request, &status_code, &bytes_sent);

        cache_release(cache, &h);

        long end_time = get_time_ms(); 
        update_stats(shm, sems, status_code, bytes_sent, end_time - start_time); 
        logger_write("127.0.0.1", req.method, req.path, status_code, (size_t)bytes_sent, end_time - start_time);
    }
    else if (access(abs_path, F_OK) != 0){
        send_error_response(client_fd, 404, "Not Found", 0);
        status_code = 404;
        bytes_sent = 0;
        long end_time = get_time_ms(); 
        update_stats(shm, sems, status_code, bytes_sent, end_time - start_time); 
        logger_write("127.0.0.1", req.method, req.path, status_code, (size_t)bytes_sent, end_time - start_time);
    }
    else if (!cache_load_file(cache, relpath, abs_path, &h)){
        struct stat st;
        if (stat(abs_path, &st) == 0 && S_ISREG(st.st_mode)) {
            size_t file_size = st.st_size;
            const char* content_type = mime_type_from_path(relpath);
            
            FILE *f = fopen(abs_path, "rb");
            if (f) {
                char *tmp_buf = malloc(file_size);
                if (tmp_buf && fread(tmp_buf, 1, file_size, f) == file_size) {
                    // Use helper to handle range/full content
                    send_content(client_fd, content_type, tmp_buf, file_size, &req, 0, is_head_request, &status_code, &bytes_sent);
                } else {
                    send_error_response(client_fd, 500, "Internal Server Error", 0);
                    status_code = 500;
                }
                if(tmp_buf) free(tmp_buf);
                fclose(f);
            } else {
                // Check errno to determine the type of error
                if (errno == EACCES) {
                    // Permission denied - return 403 Forbidden
                    send_error_response(client_fd, 403, "Forbidden", 0);
                    status_code = 403;
                    bytes_sent = 0;
                } else {
                    // Other errors (e.g., memory allocation failure) - return 500
                    send_error_response(client_fd, 500, "Internal Server Error", 0);
                    status_code = 500;
                    bytes_sent = 0;
                }
            }
        } else {
            send_error_response(client_fd, 500, "Internal Server Error", 0);
            status_code = 500;
        }
        long end_time = get_time_ms(); 
        update_stats(shm, sems, status_code, bytes_sent, end_time - start_time); 
        logger_write("127.0.0.1", req.method, req.path, status_code, (size_t)bytes_sent, end_time - start_time);
    }
    else {
        const char* content_type = mime_type_from_path(relpath);
        
        // Use helper to handle range/full content
        send_content(client_fd, content_type, (const char*)h.data, h.size, &req, 0, is_head_request, &status_code, &bytes_sent);

        cache_release(cache, &h);

        long end_time = get_time_ms(); 
        update_stats(shm, sems, status_code, bytes_sent, end_time - start_time); 
        logger_write("127.0.0.1", req.method, req.path, status_code, (size_t)bytes_sent, end_time - start_time);
    }

    close(client_fd); 
}



// Function to create a thread pool with worker threads
// Arguments:
// num_threads - Number of worker threads to create
// shm - Pointer to shared memory data for inter-process communication
// sems - Pointer to semaphores for synchronization
// Returns: Pointer to newly created thread pool, or NULL on failure

thread_pool_t* create_thread_pool(int num_threads, int max_queue_size, shared_data_t* shm, semaphores_t* sems) {

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
    pool->max_queue_size = max_queue_size;  // Store maximum queue size

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
            // Supports Keep-Alive internally
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