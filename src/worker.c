#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <signal.h>
#include "worker.h"
#include "shared_mem.h"
#include "semaphores.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include "thread_pool.h"
#include "config.h"

// ###################################################################################################################
// Sending file descriptors over UNIX sockets
// https://gist.github.com/domfarolino/4293951bd95082125f2b9931cab1de40
// ###################################################################################################################

// Function to send a file descriptor over a Unix domain socket
// socket -> Socket file descriptor
static int recv_fd(int socket){
    // msghdr -> Message header structure
    // msg -> Message to be sent
    // {0} -> Initialize all fields to ero
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
        // sizeof(int) -> Size of the file descriptor
        char buf[CMSG_SPACE(sizeof(int))];

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


    // recvmsg -> Receive message on socket
    // socket -> Socket file descriptor
    // &msg -> Address of the message header
    // 0 -> No special flags
    // < 0 -> Error occurred
    // == 0 -> Success
    if (recvmsg(socket, &msg, 0)<0){
        perror("Failed to receive fd");
        return -1;
    }

    // cmsghdr -> Control message header
    // CMSG_FIRSTHDR -> Macro to get the first control message header
    // &msg -> Address of the message header
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);

    // Extract the file descriptor from the control message
    // cmsg -> Control message header
    // CMSG_LEN -> Macro to calculate length of control message
    // sizeof(int) -> Size of an integer (file descriptor)
    if(cmsg && (cmsg->cmsg_len == CMSG_LEN(sizeof(int)))){

        // Validate the control message
        // cmsg->cmsg_level -> Level of the control message
        // SOL_SOCKET -> Socket level 
        // cmsg->cmsg_type -> Type of the control message
        // SCM_RIGHTS -> Send file descriptor
        if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS) {
            return -1;
        }

        // Return the received file descriptor
        // CMSG_DATA -> Macro to get pointer to control message data
        // (int*) -> Cast to integer pointer
        // cmsg -> Control message header
        return *((int *)CMSG_DATA(cmsg));
    }
    
    return -1;

}


// Global variable to control the worker loop
static volatile int worker_running = 1;


// Function to start the worker process
void worker_signal_handler(int signum){
    (void)signum;
    worker_running = 0;
}


// ###################################################################################################################
// FEATURE 1: Connection Queue Consumer
// ###################################################################################################################


// Worker main function
// Wait for connections in the shared memory queue 
// Block queue
// Dequeue connection
// Unblock queue
// Mark slot as empty
// Process connection

void worker_main(shared_data_t* shm, semaphores_t* sems, int worker_id, int channel_fd){

    // Register signal handler proprietary to this worker

    // signal -> Start the worker process
    // SIGTERM and SIGINT will stop the worker process
    signal(SIGTERM, worker_signal_handler);
    signal(SIGINT, worker_signal_handler);

    thread_pool_t* pool = creatwe_thread_pool(10); // Create a thread pool with 10 threads
    printf("Worker %d: Starting main loop.\n", worker_id);

    while(worker_running){

        // Wait for an available connection
        if(sem_wait(sems->filled_slots)!=0){
            
            if(!worker_running){ // Check if we need to exit
                break;
            }
            perror("Worker sem_wait error");
            continue;

        }


        // Exclusive access to queue
        sem_wait(sems->queue_mutex);

        // Dequeue client socket 
        // shm -> Shared memory structure
        // queue.sockets -> Array of client sockets in the queue
        // queue.front -> Index of the front of the queue

        // Ignore the client socket
        // shm -> Shared memory structure
        // queue.front -> Index of the front of the queue
        int ignore_fd = shm->queue.sockets[shm->queue.front];
        (void)ignore_fd; // We don't use it here since we receive via UNIX socket
        
        // Update queue front and count
        // queue.size -> Maximum size of the queue

        shm->queue.front = (shm->queue.front + 1) % MAX_QUEUE_SIZE;
        
        // Decrease the count of items in the queue
        shm->queue.count--;


        // Free mutex and signal that a slot is now free
        // sems -> queue_mutex --> Release mutex semaphore for queue
        // sems -> empty_slots --> Signal that an empty slot is available
        sem_post(sems->queue_mutex);
        sem_post(sems->empty_slots);

        // Receive the client file descriptor from the master via UNIX socket
        // channel_fd -> Channel file descriptor
        // recv_fd -> Function to receive file descriptor
        // client_fd -> Client socket file descriptor
        int client_fd = recv_fd(channel_fd);

        // Error handling
        if (client_fd < 0) {
            fprintf(stderr, "Worker %d: Falha ao receber descritor real.\n", worker_id);
            continue;
        }

        // Process the client request
        printf("Worker %d: Processing client socket %d\n", worker_id, client_fd);
        
        // Submit the client request to the thread pool
        thread_pool_submit(pool, client_fd);


        // Simulate processing time
        // buffer -> Buffer for reading client data
        char buffer[1024];

        // read -> Read data from the client socket
        // client_fd -> Client socket file descriptor
        if (read(client_fd, buffer, sizeof(buffer)) < 0) {
            perror("read failed");
        }

        // Send a simple HTTP response
        // write -> Write data to the client socket
        // client_fd -> Client socket file descriptor
        if (write(client_fd, "HTTP/1.1 200 OK\r\n\r\nOK", 19) < 0) {
            perror("write failed");
        }
        
        destroy_thread_pool(pool); // Destroy the thread pool after processing  
        close(client_fd); // Close the client socket

    }
    
    // Close the channel file descriptor
    close(channel_fd);
    printf("Worker %d: Exiting main loop.\n", worker_id);

}