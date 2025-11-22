#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <signal.h>
#include "worker.h"
#include "shared_mem.h"
#include "semaphores.h"

// Global variable to control the worker loop
static volatile int worker_running = 1;


// Function to start the worker process
void worker_signal_handler(int signum){
    (void)signum;
    worker_running = 0;
}

void worker_main(shared_data_t* shm, semaphores_t* sems, int worker_id){

    // Register signal handler proprietary to this worker

    // signal -> Start the worker process
    // SIGTERM and SIGINT will stop the worker process
    signal(SIGTERM, worker_signal_handler);
    signal(SIGINT, worker_signal_handler);

    printf("Worker %d: Starting main loop.\n", worker_id);

    while(worker_running){
        // ###################################################################################################################
        // FEATURE 1: Connection Queue Consumer
        // ###################################################################################################################

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

        int client_fd = shm->queue.sockets[shm->queue.front];
        
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

        // Process the client request
        printf("Worker %d: Processing client socket %d\n", worker_id, client_fd);
        
        
        // Simulate processing time
        // buffer -> Buffer for reading client data
        char buffer[1024];

        read(client_fd, buffer, sizeof(buffer)); // Read data from client
        write(client_fd, "HTTP/1.1 200 OK\r\n\r\nOK", 19); // Send a simple HTTP response
        close(client_fd); // Close the client socket

    }

    printf("Worker %d: Exiting main loop.\n", worker_id);

}