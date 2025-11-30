// ###################################################################################################################
// master.c — Master process of the concurrent HTTP server
//
// Responsibilities:
//  - Read configuration (server.conf)
//  - Create TCP listening socket (bind/listen)
//  - Create shared memory and semaphores (connection queue)
//  - Create N worker processes and a UNIX channel (socketpair) per worker
//  - Accept connections and distribute them (round-robin) by sending the real FD via SCM_RIGHTS
//  - Graceful shutdown on SIGINT/SIGTERM
// ###################################################################################################################

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>

#include "config.h"       // server_config_t, load_config()
#include "shared_mem.h"   // create_shared_memory(), destroy_shared_memory()
#include "semaphores.h"   // init_semaphores(), destroy_semaphores()
#include "worker.h"       // worker_init_resources(), worker_main(), worker_shutdown_resources()
#include "logger.h"       // logger_init/logger_close (Feature 5)
#include "stats.h"        // print_stats() (Feature 1: statistics tracking)

// ###################################################################################################################
// Global state and signal handlers for the master process
// ###################################################################################################################

static volatile int master_running = 1; // Control variable for main loop

static shared_data_t* g_shm = NULL; // Global shared memory pointer
static semaphores_t* g_sems = NULL; // Global semaphores pointer


void stats_timer_handler(int signum){
    (void)signum;

    if(g_shm && g_sems){
        print_stats(g_shm, g_sems); // 1 second interval
    }

    alarm(30);; // Re-arm the alarm for next 30 seconds
}
// Signal handler for graceful shutdown of the master process
static void master_signal_handler(int signum) {
    (void)signum;
    master_running = 0;
}

// ###################################################################################################################
// Utilities: TCP socket (listen) and UNIX channel (socketpair)
// ###################################################################################################################

// Create a TCP server socket (IPv4), bind and listen
// Arguments:
//  port -> TCP port
// Return:
//  socket descriptor on success; -1 on error
static int create_listen_socket(int port) {

    // socket(AF_INET, SOCK_STREAM, 0) -> TCP IPv4
    int s = socket(AF_INET, SOCK_STREAM, 0);

    // Check for errors
    if (s < 0) {
        perror("socket");
        return -1;
    }

    // SO_REUSEADDR -> allow quick reuse of the port after restart
    int yes = 1; // option value

    // setsockopt -> Set socket options
    // SOL_SOCKET: socket level 
    //SO_REUSEADDR: option name
    if (setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0) { 

        // Error setting option
        perror("setsockopt(SO_REUSEADDR)");
        close(s);
        return -1;

    }

    // bind to 0.0.0.0:port
    struct sockaddr_in addr; // IPv4 address structure

    memset(&addr, 0, sizeof(addr)); // Zero out structure

    addr.sin_family      = AF_INET; // IPv4
    addr.sin_addr.s_addr = htonl(INADDR_ANY); // htonl -> network byte order
    addr.sin_port        = htons((uint16_t)port); // htons -> network byte order


    // bind -> Bind socket to address/port
    // socket descriptor, address, address length
    if (bind(s, (struct sockaddr*)&addr, sizeof(addr)) < 0) { // Check for errors
        perror("bind");
        close(s);
        return -1;
    }

    // listen with a reasonable backlog
    // listen -> Mark socket as passive (listening)
    if (listen(s, 1024) < 0) { // Check for errors
        perror("listen");
        close(s);
        return -1;
    }

    return s;
}

// Create a master<->worker channel using socketpair AF_UNIX (DGRAM)
// Arguments:
//  sv[2] -> returns the two descriptors (sv[0] stays in master, sv[1] goes to worker)
// Return:
//  0 on success; -1 on error
static int create_worker_channel(int sv[2]) { // Create socketpair

    // socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) -> UNIX domain datagram socketpair
    // Check for errors
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) < 0) { // Create socketpair
        perror("socketpair");
        return -1;
    }
    return 0;
}

// Send client descriptor to a worker via SCM_RIGHTS
// Arguments:
//  socket -> master's end (sv[0])
//  fd     -> client descriptor accepted on listen
// Return:
//  0 on success; -1 on error

// Sends a file descriptor over a UNIX domain socket using SCM_RIGHTS.
static int send_fd(int socket, int fd) {

    struct msghdr msg = {0}; // Message header
    char buf[1] = {'F'}; // mandatory "dummy" payload
    struct iovec io = { .iov_base = buf, .iov_len = 1 }; // I/O vector

    union { // Control message buffer
        char buf[CMSG_SPACE(sizeof(int))]; // Space for one FD
        struct cmsghdr align; // Alignment
    } u;

    memset(&u, 0, sizeof(u)); // Zero out control buffer

    // Set up message header
    msg.msg_iov = &io; // msg_iov -> I/O vector
    msg.msg_iovlen = 1; // One I/O vector
    msg.msg_control = u.buf; // Control message buffer
    msg.msg_controllen = sizeof(u.buf); // Control message length

    // cmsg header
    struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg); // First control message
    cmsg->cmsg_level = SOL_SOCKET; // Socket level
    cmsg->cmsg_type  = SCM_RIGHTS; // Type: passing FDs
    cmsg->cmsg_len   = CMSG_LEN(sizeof(int)); // Length

    *((int*) CMSG_DATA(cmsg)) = fd; // Set the FD to pass

    // sendmsg -> Send message
    if (sendmsg(socket, &msg, 0) < 0) { // Check for errors
        perror("sendmsg(SCM_RIGHTS)");
        return -1;
    }

    return 0;
}

// ###################################################################################################################
// Initialization of Shared Memory and Semaphores (uses the real API of your project)
// ###################################################################################################################

/**
 * Initializes the shared memory segment for inter-process communication.
 *
 * This function creates and initializes a shared memory segment of the specified
 * queue size by calling create_shared_memory(). It returns a pointer to the
 * allocated shared_data_t structure on success, or NULL on failure.
 *
 * Real shared memory: create_shared_memory(queue_size)
 *
 * queue_size -> The size of the queue to be allocated in shared memory.
 * return -> Pointer to the initialized shared_data_t structure, or NULL if creation fails.
 *
 * Steps:
 * 1. Calls create_shared_memory() with the given queue_size to allocate shared memory.
 * 2. Checks if the allocation was successful.
 *    - If not, prints an error message to stderr and returns NULL.
 * 3. Returns the pointer to the allocated shared memory structure.
 */
static shared_data_t* init_shared_memory(int queue_size) {

    // It is expected that the shared_mem module handles shm_open/ftruncate/mmap/init of the queue (front=0,count=0)
    shared_data_t* shm = create_shared_memory(queue_size);
    if (!shm) {
        fprintf(stderr, "MASTER: shared_mem_create() failed.\n");
        return NULL;
    }
    return shm;
}

// Real Semaphores: init_semaphores(), destroy_semaphores()
static semaphores_t* init_semaphore_system(int queue_size) {

    semaphores_t* sems = (semaphores_t*)malloc(sizeof(semaphores_t));
    if (!sems) {
        fprintf(stderr, "MASTER: malloc failed for semaphores\n");
        return NULL;
    }

    if (init_semaphores(sems, queue_size) != 0) {
        fprintf(stderr, "MASTER: init_semaphores() failed\n");
        free(sems);
        return NULL;
    }

    return sems;
}
// ###################################################################################################################
// Enqueue in the shared queue (signaling capacity/count)
// The worker ignores the stored value (since it receives the real FD via SCM_RIGHTS).
// ###################################################################################################################

// Returns 0 on success; -1 on error
static int enqueue_connection(shared_data_t* shm, semaphores_t* sems, int placeholder_fd) {

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
    shm->queue.sockets[pos] = placeholder_fd; // marker (will not be used by the worker)
    shm->queue.count++;

    // Release critical section
    sem_post(sems->queue_mutex);

    // Signal available item
    sem_post(sems->filled_slots);

    return 0;
}

// ###################################################################################################################
// Main function of the master process
// ###################################################################################################################

int main(int argc, char* argv[]) {

    // ---------------------------------------------------------------------------------------------------------------
    // 1) Load configuration
    // ---------------------------------------------------------------------------------------------------------------
    const char* conf_path = (argc >= 2) ? argv[1] : "server.conf"; // Config file path

    server_config_t config; // Server configuration structure

    memset(&config, 0, sizeof(config)); // Zero out structure

    // Set reasonable defaults in case config file loading fails
    config.port               = 8080; // Default port
    config.document_root[0]   = '\0'; // Will be set below
    config.num_workers        = 2; // Default number of workers
    config.threads_per_worker = 10; // Default threads per worker
    config.max_queue_size     = MAX_QUEUE_SIZE; // Default max queue size
    config.log_file[0]        = '\0'; // Will be set below
    config.cache_size_mb      = 64; // Default cache size in MB
    config.timeout_seconds    = 30; // Default timeout in seconds


    signal(SIGALRM, stats_timer_handler); // Set up alarm signal handler
    alarm(30); // Schedule first alarm in 30 seconds

    // Use safer string copy for defaults
    strncpy(config.document_root, "www", sizeof(config.document_root) - 1);
    config.document_root[sizeof(config.document_root) - 1] = '\0'; // Ensure null termination
    strncpy(config.log_file, "logs/access.log", sizeof(config.log_file) - 1);
    config.log_file[sizeof(config.log_file) - 1] = '\0'; // Ensure null termination

    // Load configuration from file
    if (load_config(conf_path, &config) != 0) {
        fprintf(stderr, "MASTER: Using defaults (failed to load %s)\n", conf_path); // Error loading config
    } else {
        fprintf(stderr, "MASTER: Config loaded from %s\n", conf_path); // Success
    }

    // ADDED: initialize the global logger (Feature 5)
    logger_init(config.log_file); // Initialize logger

    // ---------------------------------------------------------------------------------------------------------------
    // 2) Signal handlers (CTRL+C, kill)
    // ---------------------------------------------------------------------------------------------------------------
    signal(SIGINT,  master_signal_handler); // Handle CTRL+C
    signal(SIGTERM, master_signal_handler); // Handle termination signal

    // ---------------------------------------------------------------------------------------------------------------
    // 3) Shared memory and semaphores
    // ---------------------------------------------------------------------------------------------------------------
    shared_data_t* shm = init_shared_memory(config.max_queue_size); // Initialize shared memory
    
    // Check for errors
    if (!shm) {
        return 1;
    }

    semaphores_t* sems = init_semaphore_system(config.max_queue_size); // Initialize semaphores

    // Check for errors
    if (!sems) {
        destroy_shared_memory(shm);
        return 1;
    }

    g_shm = shm; // For stats printing
    g_sems = sems; // For stats printing
    // ---------------------------------------------------------------------------------------------------------------
    int listen_fd = create_listen_socket(config.port); // Create listen socket

    if (listen_fd < 0) { // Check for errors
        destroy_semaphores(sems); // Cleanup semaphores
        free(sems);
        destroy_shared_memory(shm); // Cleanup shared memory
        return 1;
    }

    // ---------------------------------------------------------------------------------------------------------------
    // 5) Create N workers (fork) and a UNIX channel per worker
    // ---------------------------------------------------------------------------------------------------------------
    int num_workers = (config.num_workers > 0) ? config.num_workers : 1; // Ensure at least 1 worker

    pid_t* pids       = (pid_t*)calloc((size_t)num_workers, sizeof(pid_t)); // Array of worker PIDs
    int*   parent_end = (int*)  calloc((size_t)num_workers, sizeof(int)); // Array of master's ends of channels

    // Check for allocation errors
    if (!pids || !parent_end) {
        fprintf(stderr, "MASTER: alloc failed\n"); // Allocation failure

        close(listen_fd); // Close listen socket

        destroy_semaphores(sems); // Cleanup semaphores
        free(sems);

        destroy_shared_memory(shm); // Cleanup shared memory

        // Cleanup allocated arrays
        free(pids); 
        free(parent_end);
        return 1;
    }

    // Create N worker processes, each with its own UNIX domain socketpair for communication
    for (int i = 0; i < num_workers; ++i) { // For each worker
        int sv[2]; // socketpair descriptors: sv[0] for master, sv[1] for worker

        // Create UNIX domain socketpair for master<->worker communication
        if (create_worker_channel(sv) != 0) {
            fprintf(stderr, "MASTER: failed to create channel for worker %d\n", i); // Error

            // partial cleanup
            for (int k = 0; k < i; ++k) close(parent_end[k]); // close already created channels

            close(listen_fd); // close listen socket
            destroy_semaphores(sems); // destroy semaphores
            free(sems);
            destroy_shared_memory(shm); // destroy shared memory
            
            // free allocated arrays
            free(pids); 
            free(parent_end);
            return 1;
        }


        pid_t pid = fork(); // Fork worker process

        if (pid < 0) {
            perror("fork");

            close(sv[0]); // close both ends of the channel 
            close(sv[1]);  // close listen socket
            for (int k = 0; k < i; ++k) close(parent_end[k]); // close already created channels
            close(listen_fd); // close listen socket
            destroy_semaphores(sems); // destroy semaphores
            free(sems);
            destroy_shared_memory(shm); // destroy shared memory
            free(pids); free(parent_end); // free allocated arrays
            return 1;
        }

        if (pid == 0) {
            // -------------------------------------------------------------------------------------------------------
            // CHILD process (WORKER)
            // -------------------------------------------------------------------------------------------------------
            // Close master's end in this process
            close(sv[0]);

            // The worker does not need the listen_fd
            close(listen_fd);

            // Initialize worker resources (e.g., per-worker cache)
            worker_init_resources(&config);

            // Enter the main loop of the worker
            worker_main(shm, sems, i, sv[1]);

            // Note: worker_main already calls worker_shutdown_resources() at the end
            _exit(0);
        }

        // -----------------------------------------------------------------------------------------------------------
        // PARENT process (MASTER)
        // -----------------------------------------------------------------------------------------------------------
        pids[i] = pid;
        parent_end[i] = sv[0]; // master keeps this end
        close(sv[1]);          // close the worker's end in the master
    }

    fprintf(stderr, "MASTER: listening on port %d with %d workers.\n", config.port, num_workers);

    // ---------------------------------------------------------------------------------------------------------------
    // 6) Main event loop: Accept incoming connections and distribute them to workers in round-robin fashion
    // ---------------------------------------------------------------------------------------------------------------
    int rr = 0; // Round-robin index for worker selection
    while (master_running) {

        // Accept a new client connection (blocking call)
        struct sockaddr_in cli; // Client address structure
        socklen_t cli_len = sizeof(cli); // Length of client address
        int client_fd = accept(listen_fd, (struct sockaddr*)&cli, &cli_len); // Accept connection

        // Check for errors
        if (client_fd < 0) {
            if (errno == EINTR && !master_running) break; // interrupted by signal
            perror("accept");
            continue;
        }

        // (Optional) Make non-blocking:
        // int flags = fcntl(client_fd, F_GETFL, 0);
        // fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);

        // Enqueue marker in the shared queue (capacity/count)
        if (enqueue_connection(shm, sems, client_fd) != 0) {
            // If the queue is full or semaphore error, reject connection
            close(client_fd);
            continue;
        }

        // Choose worker by round-robin
        int w = rr;
        rr = (rr + 1) % num_workers;

        // Send the real FD to worker “w” via SCM_RIGHTS
        if (send_fd(parent_end[w], client_fd) != 0) {
            // If sending fails, close locally
            close(client_fd);
            // (Optional) log statistics/error
        }

        // The master no longer needs the FD after sending it
        close(client_fd);
    }

    // ---------------------------------------------------------------------------------------------------------------
    // 7) Graceful shutdown
    // ---------------------------------------------------------------------------------------------------------------
    fprintf(stderr, "MASTER: shutting down...\n");

    // Close listen socket
    close(listen_fd);

    // Close channels and signal workers
    for (int i = 0; i < num_workers; ++i) {
        close(parent_end[i]);
        if (pids[i] > 0) kill(pids[i], SIGTERM);
    }

    // Wait for workers to terminate
    for (int i = 0; i < num_workers; ++i) {
        if (pids[i] > 0) {
            int status = 0;
            waitpid(pids[i], &status, 0);
        }
    }

    // Release master's resources
    destroy_semaphores(sems);
    free(sems);
    destroy_shared_memory(shm);
    free(pids);
    free(parent_end);

    // ADDED: close the global logger
    logger_close();

    fprintf(stderr, "MASTER: bye.\n");
    return 0;
}
