#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/wait.h>
#include <errno.h>
#include <string.h>
#include "shared_mem.h"
#include "semaphores.h"
#include "config.h" 
#include "worker.h" 
#include "stats.h"

#define PORT 8080 
#define NUM_WORKERS 4

// Global variable to control the main loop
volatile sig_atomic_t keep_running = 1;

shared_data_t* g_shm = NULL; // Global pointer to shared memory
semaphores_t* g_sems = NULL; // Global pointer to semaphores

// Signal handler to print statistics periodically
void alarm_handler(int signum) { 
    (void)signum; // Avoid unused parameter warning
    if(g_shm && g_sems) { // Check if global pointers are set
        print_stats(g_shm, g_sems); // Print statistics
    }
    alarm(30); // Schedule next alarm in 30 seconds
}

// Signal handler to gracefully shut down the server
void signal_handler(int signum __attribute__((unused))) {
    keep_running = 0;
}

// ###################################################################################################################
// Sending file descriptors over UNIX sockets
// https://gist.github.com/domfarolino/4293951bd95082125f2b9931cab1de40
// ###################################################################################################################


// Function to send a file descriptor over a Unix domain socket
// socket -> Socket file descriptor
// fd_to_send -> File descriptor to send

static int send_fd(int socket, int fd_to_send){
    // msghdr -> Message header structure
    // msg -> Message to be sent
    // {0} -> Initialize all fields to zero
    struct msghdr msg = {0}; 
    
    // buf -> Buffer to hold a single byte (required for sendmsg)
    char buf[1];

    // iovec -> I/O vector structure
    // io -> I/O vector for the message
    // .iov_base -> Pointer to the buffer
    // .iov_len -> Length of the buffer
    struct iovec io = { .iov_base = buf, .iov_len = 1 };

    // Union to hold control message
    union {

        // buf -> Buffer for control message
        // CMSG_SPACE -> Macro to calculate space needed for control message
        // sizeof(fd_to_send) -> Size of the file descriptor
        char buf[CMSG_SPACE(sizeof(fd_to_send))];

        // align -> cmsghdr structure for alignment
        // cmsghdr -> Control message header structure
        struct cmsghdr align;
    } u; // u -> Union to hold control message


    // Set up the message header
    // msg.msg_iov -> Pointer to the I/O vector
    // &io -> Address of the I/O vector
    msg.msg_iov = &io;

    // msg.msg_iovlen -> Number of I/O vectors
    // 1 -> Number of I/O vectors
    msg.msg_iovlen = 1;

    // msg.msg_control -> Pointer to control message buffer
    // u.buf -> Address of the control message buffer
    msg.msg_control = u.buf;

    // msg.msg_controllen -> Length of control message buffer
    // sizeof(u.buf) -> Size of the control message buffer
    msg.msg_controllen = sizeof(u.buf);


    // cmsghdr -> Control message header
    // CMSG_FIRSTHDR -> Macro to get the first control message header
    // &msg -> Address of the message header
    struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);

    // Set up the control message header
    // cmsg->cmsg_level -> Level of the control message
    // SOL_SOCKET -> Socket level
    cmsg->cmsg_level = SOL_SOCKET;

    // cmsg->cmsg_type -> Type of the control message
    // SCM_RIGHTS -> Send file descriptor
    cmsg->cmsg_type = SCM_RIGHTS;

    // cmsg->cmsg_len -> Length of the control message
    // CMSG_LEN -> Macro to calculate length of control message
    // sizeof(fd_to_send) -> Size of the file descriptor
    cmsg->cmsg_len = CMSG_LEN(sizeof(fd_to_send));


    // Copy the file descriptor to the control message data
    // CMSG_DATA -> Macro to get pointer to control message data
    // (int*) -> Cast to integer pointer
    // fd_to_send -> File descriptor to send
    *((int*) CMSG_DATA(cmsg)) = fd_to_send;


    // Send the message
    // sendmsg -> Send message on socket
    // socket -> Socket file descriptor
    // &msg -> Address of the message header
    // 0 -> No special flags
    // < 0 -> Error occurred
    // == 0 -> Success
    if (sendmsg(socket, &msg, 0)<0){
        perror("Failed to send fd");
        return -1;
    }

    return 0;
}
// Function to send 503 Service Unavailable response
void send_503(int client_fd) {

    // Prepare and send the 503 response
    const char* response = 
        "HTTP/1.1 503 Service Unavailable\r\n"
        "Content-Type: text/plain\r\n"
        "Connection: close\r\n"
        "\r\n"
        "Server is busy. Please try again later.";

    // Send the response
    if (write(client_fd, response, strlen(response)) < 0) {
        perror("write failed");
    }

    // Close the client connection
    close(client_fd);
}

// Function to create and bind the server socket
int create_server_socket(int port) {

    // Create socket
    // AF_INET -> IPv4
    // SOCK_STREAM -> TCP
    // 0 -> Default protocol (TCP for SOCK_STREAM)
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);

    // Error handling
    if (sockfd < 0) {
        perror("socket creation failed");
        return -1;
    }


    // Set socket options
    //opt --> Option value
    //opt = 1 --> Enable the option
    int opt = 1;

    // setsockopt -> Set options on the socket
    // sockfd -> Socket file descriptor
    // SOL_SOCKET -> Level for socket options
    // SO_REUSEADDR -> Allow reuse of local addresses
    // &opt -> Pointer to the option value
    // sizeof(opt) -> Size of the option value

    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt failed");
        close(sockfd);
        return -1;
    }

    // Initialize address structure
    struct sockaddr_in addr;

    // memset -> Fill memory with a constant byte
    // &addr -> Pointer to the memory area
    // 0 -> Byte value to set
    // sizeof(addr) -> Number of bytes to set
    memset(&addr, 0, sizeof(addr));

    // Configure address
    // addr.sin_family -> Address family (IPv4)
    // addr.sin_addr.s_addr -> Accept connections from any IP address
    // addr.sin_port -> Port number (in network byte order)
    // htons -> Convert port number to network byte order
    // PORT -> Port number to listen on
    // INADDR_ANY -> Accept connections from any IP address
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);


    // Bind socket to address
    // bind -> Bind the socket to the specified address
    // sockfd -> Socket file descriptor
    // (struct sockaddr*)&addr -> Pointer to the address structure
    // sizeof(addr) -> Size of the address structure

    // Error handling    
    if (bind(sockfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind failed");
        close(sockfd);
        return -1;
    }

    // Start listening for incoming connections
    // listen -> Mark the socket as a passive socket to accept incoming connections
    // sockfd -> Socket file descriptor
    // 128 -> Maximum length of the pending connections queue

    // Error handling
    // listen -> Start listening on the socket
    // 128 -> Maximum length of the pending connections queue
    // < 0 -> Error occurred
    // == 0 -> Success

    if (listen(sockfd, 128) < 0) {
        perror("listen failed");
        close(sockfd);
        return -1;
    }

    return sockfd;
}

// Function to enqueue a connection into the shared memory queue
// This is the producer function in the producer-consumer model
// Receive a client 
// Check if the queue is full
// If full, send 503 response and close the connection
// If not full, enqueue the connection and signal the worker
// Put the client_fd into the shared memory queue
// Free mutex and signal that a new connection is available

void enqueue_connection(shared_data_t* data, semaphores_t* sems, int client_fd, int channel_fd){

    // Check if queue is full
    // sem_trywait -> Try to acquire an empty slot in the queue
    // sems -> empty_slots --> Pointer to the semaphore counting empty slots in the queue
    if(sem_trywait(sems->empty_slots) != 0){
        // Queue is full
        send_503(client_fd);
        return;
    }

    // Wait for an empty slot in the queue
    sem_wait(sems->queue_mutex);

    // Enqueue the client file descriptor
    // data -> Shared memory structure
    // queue.sockets -> Array of client sockets in the queue
    // queue.rear -> Index of the rear of the queue

    data->queue.sockets[data->queue.rear] = client_fd;
    data->queue.rear = (data->queue.rear + 1) % MAX_QUEUE_SIZE;
    data->queue.count++;

    // Send the client file descriptor to the worker via UNIX socket
    // channel_fd -> Channel file descriptor
    // client_fd -> Client file descriptor to send
    if(send_fd(channel_fd, client_fd) < 0){
        perror("Failed to send fd to worker");
    }

    // Close the client file descriptor in the master process
    close(client_fd);

    // Release mutex and signal that a new connection is available
    sem_post(sems->queue_mutex);
    sem_post(sems->filled_slots);
}


int main() {
    // Create shared memory
    shared_data_t* shm = create_shared_memory();

    if(!shm){
        perror("Failed to create shared memory");
        exit(1);
    }

    // Create semaphores
    semaphores_t sems;
    if(init_semaphores(&sems, MAX_QUEUE_SIZE) < 0) {
        perror("Failed to initialize semaphores");
        destroy_shared_memory(shm);
        exit(1);
    }

    g_shm = shm; // Set global shared memory pointer
    g_sems = &sems; // Set global semaphores pointer

    // Set alarm signal handler for periodic statistics printing
    signal(SIGALRM, alarm_handler);
    alarm(30); // Schedule first alarm in 30 seconds


    // Configure signal handlers for graceful shutdown
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    int sv[2];  // sv[0] = Master escreve
                // sv[1] = Worker lê


    // Create UNIX socket pair for communication between master and workers
    // socketpair -> Create a pair of connected sockets
    // AF_UNIX -> Address family for Unix domain sockets
    // SOCK_DGRAM -> Datagram socket
    // 0 -> Default protocol (UDP for SOCK_DGRAM)
    // sv -> Array to hold the two socket file descriptors
    // https://gist.github.com/domfarolino/4293951bd95082125f2b9931cab1de40

    if(socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) < 0){
        perror("Failed to create socket pair");
        exit(1);
    }

    // Create worker threads (using fork)

    pid_t workers[NUM_WORKERS];

    for(int i=0; i<NUM_WORKERS; i++){
        pid_t pid = fork();

        if(pid < 0){
            perror("Failed to fork worker process");
            exit(1);
        } else if (pid == 0){
            // Child process - Worker
            // Close master's end in worker process
            close(sv[0]); 

            // Start worker main function
            worker_main(shm, &sems, i, sv[1]);
            exit(0);
        } 
        else {
            // Parent process
            workers[i] = pid;
        }
        
    }

    close(sv[1]); // Close worker's end in master process

    // Producer - Master process
    int server_fd = create_server_socket(PORT);

    // Error handling
    if(server_fd < 0){
        perror("Failed to create server socket");
        keep_running = 0;
    } else {
        printf("Server listening on port %d\n", PORT);
    }


    // Accept loop 
    while(keep_running){
        // Accept connection
        int client_fd = accept(server_fd, NULL, NULL);

        // Error handling
        if (client_fd < 0) {
            // Check if the error is due to interruption by signal
            if(errno == EINTR){
                break;
            }
            // Other errors
            perror("accept failed");
            continue;
        }

        // ###################################################################################################################
        // FEATURE 1: Connection Queue (Producer - Consumer Model)
        // ###################################################################################################################
    
        // Enqueue the connection using the dedicated function
        enqueue_connection(shm, &sems, client_fd, sv[0]);
    }

    // Cleanup before exiting
    printf("Shutting down server...\n");

    // server_fd -> File descriptor for the server socket
    // >= 0 -> Valid file descriptor
    if(server_fd >= 0){
        close(server_fd);
    }

    // Terminate worker processes

    for(int i=0; i<NUM_WORKERS; i++){

        // workers[i] -> PID of the worker process
        // SIGTERM -> Signal to terminate the process

        kill(workers[i], SIGTERM);

        // Wait for worker process to exit
        // workers[i] -> PID of the worker process
        // NULL -> No need to retrieve exit status
        // 0 -> No special options

        waitpid(workers[i], NULL, 0);
    }

    // Destroy semaphores and shared memory
    destroy_semaphores(&sems);
    destroy_shared_memory(shm);
    

    return 0;
    }