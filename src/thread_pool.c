#include "thread_pool.h"
#include "stats.h"
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <strings.h>   // strcasecmp
#include <stdio.h>

#include "worker.h"    // worker_get_cache(), worker_get_document_root()
#include "cache.h"     // file_cache_t (Feature 4: cache por worker)
#include "logger.h"    // logger_write (Feature 5: logging thread-safe/process-safe)

// ----------------------------------------------------------------------------------------
// Forward declarations (não tens http_parser.h / http_builder.h, por isso declaramos aqui)
// ----------------------------------------------------------------------------------------

// Estrutura mínima usada pelo parser HTTP (compatível com http_parser.c)
typedef struct {
    char method[8];
    char path[512];
    char version[16];
} http_request_t;

// parse_http_request -> Implementada em http_parser.c
int parse_http_request(const char* raw, http_request_t* out_req);

// send_http_response -> Implementada em http_builder.c
void send_http_response(int fd, int status, const char* status_msg,
                        const char* content_type, const char* body, size_t body_len);

// ----------------------------------------------------------------------------------------
// Helper: determinar MIME type a partir da extensão do ficheiro
// ----------------------------------------------------------------------------------------
static const char* mime_type_from_path(const char* path){
    // path -> Caminho do recurso solicitado (ex.: "/index.html")
    const char* ext = strrchr(path, '.'); // procurar o último '.'
    if (!ext) return "application/octet-stream";

    ext++; // avançar para depois do '.'

    if (!strcasecmp(ext, "html")) return "text/html";
    if (!strcasecmp(ext, "htm"))  return "text/html";
    if (!strcasecmp(ext, "css"))  return "text/css";
    if (!strcasecmp(ext, "js"))   return "application/javascript";
    if (!strcasecmp(ext, "png"))  return "image/png";
    if (!strcasecmp(ext, "jpg"))  return "image/jpeg";
    if (!strcasecmp(ext, "jpeg")) return "image/jpeg";
    if (!strcasecmp(ext, "gif"))  return "image/gif";
    if (!strcasecmp(ext, "svg"))  return "image/svg+xml";
    if (!strcasecmp(ext, "json")) return "application/json";

    return "application/octet-stream"; // fallback razoável
}


// ###################################################################################################################
// FEATURE 2 + 4 + 5: HTTP Handler com Cache LRU e Logging
// ###################################################################################################################

void handle_client_request(int client_fd, shared_data_t* shm, semaphores_t* sems){
    long start_time = get_time_ms(); // Get the start time for processing

    char buffer[8192]; // Buffer to hold client request data
    int bytes_sent = 0; // Variable to track bytes sent
    int status_code = 500; // HTTP status code (default: 500)

    // Ler pedido HTTP do cliente
    // read -> Lê bytes do socket do cliente para o buffer
    ssize_t n = read(client_fd, buffer, sizeof(buffer)-1);

    // Verificar erros ou conexão fechada
    if (n <= 0){
        close(client_fd); // Close the client connection
        return;
    }

    buffer[n] = '\0'; // Null-terminate para operar como string

    // ----------------------------------------------------------------------------------------
    // PARSE DO PEDIDO HTTP (http_parser.c)
    // ----------------------------------------------------------------------------------------
    http_request_t req;

    // parse_http_request -> extrai method, path e version da primeira linha
    if (parse_http_request(buffer, &req) != 0){
        // Pedido malformado → 400 Bad Request
        const char* msg = "<h1>400 Bad Request</h1>";
        send_http_response(client_fd, 400, "Bad Request", "text/html", msg, strlen(msg));
        status_code = 400;
        bytes_sent = (int)strlen(msg);

        long end_time = get_time_ms(); // Get the end time for processing
        update_stats(shm, sems, status_code, bytes_sent, end_time - start_time); // Update statistics

        // Logging (Feature 5)
        logger_write("127.0.0.1", "?", "?", status_code, (size_t)bytes_sent, end_time - start_time);

        close(client_fd); // Close the client connection
        return;
    }

    // ----------------------------------------------------------------------------------------
    // SUPORTAMOS APENAS GET NESTA FASE
    // ----------------------------------------------------------------------------------------
    if (strcmp(req.method, "GET") != 0){
        const char* msg = "<h1>405 Method Not Allowed</h1>";
        send_http_response(client_fd, 405, "Method Not Allowed", "text/html", msg, strlen(msg));
        status_code = 405;
        bytes_sent = (int)strlen(msg);

        long end_time = get_time_ms(); // Get the end time for processing
        update_stats(shm, sems, status_code, bytes_sent, end_time - start_time); // Update statistics

        // Logging (Feature 5)
        logger_write("127.0.0.1", req.method, req.path, status_code, (size_t)bytes_sent, end_time - start_time);

        close(client_fd); // Close the client connection
        return;
    }

    // ----------------------------------------------------------------------------------------
    // CONSTRUIR CAMINHO ABSOLUTO (document_root + path)
    // ----------------------------------------------------------------------------------------

    // Obter document_root do worker
    const char* docroot = worker_get_document_root();

    // Se o cliente pediu "/", servimos "/index.html"
    const char* relpath = (strcmp(req.path, "/") == 0) ? "/index.html" : req.path;

    // abs_path -> Caminho absoluto para o ficheiro no disco
    char abs_path[1024];
    snprintf(abs_path, sizeof(abs_path), "%s%s", docroot, relpath);

    // ----------------------------------------------------------------------------------------
    // CACHE LRU (Feature 4): tentar HIT primeiro; se MISS, carregar e inserir
    // ----------------------------------------------------------------------------------------

    // Obter instância de cache deste worker
    file_cache_t* cache = worker_get_cache();

    // cache_handle_t -> “alça” de acesso aos dados em cache (mantém refcount)
    cache_handle_t h;

    // 1) Tentar HIT
    if (cache_acquire(cache, relpath, &h)){
        // Determinar MIME type do recurso
        const char* content_type = mime_type_from_path(relpath);

        // Enviar resposta HTTP a partir do conteúdo em cache
        send_http_response(client_fd, 200, "OK", content_type, (const char*)h.data, h.size);

        status_code = 200; // Update status code
        bytes_sent = (int)h.size; // Update bytes sent

        // Libertar referência do item em cache
        cache_release(cache, &h);

        // Atualizar estatísticas
        long end_time = get_time_ms(); // Get the end time for processing
        update_stats(shm, sems, status_code, bytes_sent, end_time - start_time); // Update statistics

        // Logging (Feature 5)
        logger_write("127.0.0.1", req.method, req.path, status_code, (size_t)bytes_sent, end_time - start_time);

        close(client_fd); // Close the client connection
        return;
    }

    // 2) MISS: verificar existência do ficheiro
    if (access(abs_path, F_OK) != 0){
        // Ficheiro não existe → 404
        const char* msg = "<h1>404 Not Found</h1>";
        send_http_response(client_fd, 404, "Not Found", "text/html", msg, strlen(msg));
        status_code = 404;
        bytes_sent = (int)strlen(msg);

        long end_time = get_time_ms(); // Get the end time for processing
        update_stats(shm, sems, status_code, bytes_sent, end_time - start_time); // Update statistics

        // Logging (Feature 5)
        logger_write("127.0.0.1", req.method, req.path, status_code, (size_t)bytes_sent, end_time - start_time);

        close(client_fd); // Close the client connection
        return;
    }

    // 3) MISS com ficheiro existente: carregar do disco e inserir na cache
    if (!cache_load_file(cache, relpath, abs_path, &h)){
        // Falhou leitura → 500
        const char* msg = "<h1>500 Internal Server Error</h1>";
        send_http_response(client_fd, 500, "Internal Server Error", "text/html", msg, strlen(msg));
        status_code = 500;
        bytes_sent = (int)strlen(msg);

        long end_time = get_time_ms(); // Get the end time for processing
        update_stats(shm, sems, status_code, bytes_sent, end_time - start_time); // Update statistics

        // Logging (Feature 5)
        logger_write("127.0.0.1", req.method, req.path, status_code, (size_t)bytes_sent, end_time - start_time);

        close(client_fd); // Close the client connection
        return;
    }

    // 4) Após carregar: enviar resposta com o conteúdo agora em cache
    {
        const char* content_type = mime_type_from_path(relpath);

        send_http_response(client_fd, 200, "OK", content_type, (const char*)h.data, h.size);

        status_code = 200; // Update status code
        bytes_sent = (int)h.size; // Update bytes sent

        // Libertar referência do item em cache
        cache_release(cache, &h);

        // Atualizar estatísticas
        long end_time = get_time_ms(); // Get the end time for processing
        update_stats(shm, sems, status_code, bytes_sent, end_time - start_time); // Update statistics

        // Logging (Feature 5)
        logger_write("127.0.0.1", req.method, req.path, status_code, (size_t)bytes_sent, end_time - start_time);
    }

    // Fechar a conexão do cliente
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
