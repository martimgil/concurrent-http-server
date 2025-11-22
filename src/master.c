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

#define PORT 8080 
#define NUM_WORKERS 4

volatile sig_atomic_t keep_running = 1;

void signal_handler(int signum) {
    keep_running = 0;
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
    write(client_fd, response, strlen(response));

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
void enqueue_connection(shared_data_t* data, semaphores_t* sems, int client_fd) {

    // Wait for an empty slot in the queue
    sem_wait(sems->empty_slots);
    sem_wait(sems->queue_mutex);

    // Enqueue the client file descriptor
    // data -> Shared memory structure
    // queue.sockets -> Array of client sockets in the queue
    // queue.rear -> Index of the rear of the queue

    data->queue.sockets[data->queue.rear] = client_fd;
    data->queue.rear = (data->queue.rear + 1) % MAX_QUEUE_SIZE;
    data->queue.count++;


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

    // Configure signal handlers for graceful shutdown
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);


    // Create worker threads (using fork)

    pid_t workers[NUM_WORKERS];

    for(int i=0; i<NUM_WORKERS; i++){
        pid_t pid = fork();

        if(pid < 0){
            perror("Failed to fork worker process");
            exit(1);
        } else if (pid == 0){
            // Child process - Worker
            worker_main(shm, &sems,i);
            exit(0);
        } 
        else {
            // Parent process
            workers[i] = pid;
        }
        
    }

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
    
        // Check if queue is full

        if(sem_trywait(sems.empty_slots) != 0){ //sem_trywait -> Used to avoid blocking the master process
            // If queue is full, send 503 response
            printf("Connection queue full. Sending 503 to client.\n");
            send_503(client_fd);
            continue;
        }

        // Exclusive access to queue

        //sem_wait -> Block until semaphore is available
        // sems.queue_mutex is a pointer to sem_t, so we need to dereference it
        
        sem_wait(sems.queue_mutex); 

        // Enqueue connection

        // shm is a pointer to shared_data_t
        // queue.sockets is an array of integers (client file descriptors)
        // shm -> queue.rear  --> Index to insert the new client_fd
        // client_fd --> New client file descriptor to be added to the queue

        shm->queue.sockets[shm->queue.rear] = client_fd;

        // Update rear index in a circular manner
        // queue.rear -> Is the index where the next client will be inserted
        // MAX_QUEUE_SIZE -> Size of the circular buffer
        // shm -> queue.rear + 1 --> Move to the next index
        // % MAX_QUEUE_SIZE --> Wrap around if it exceeds the buffer size

        shm->queue.rear = (shm->queue.rear + 1) % MAX_QUEUE_SIZE; 

        // Increment count of connections in the queue
        // shm -> queue.count --> Current number of connections in the queue
        shm -> queue.count++;


        // Release mutex and signal that a new connection is available
        // sem_post -> Increment the semaphore value
        //sems.queue_mutex -> Pointer to the mutex semaphore for the queue
        sem_post(sems.queue_mutex);

        // sems.filled_slots -> Pointer to the semaphore that counts filled slots in the queue
        sem_post(sems.filled_slots); // Awake a worker if it's waiting for connections
    
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
