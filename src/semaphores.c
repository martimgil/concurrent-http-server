#include "semaphores.h"
#include <fcntl.h>

// Function to initialize semaphores for the server
// Arguments:
// sems - Pointer to the semaphores_t structure to be initialized
// queue_size - Maximum size of the request queue

int init_semaphores(semaphores_t* sems, int queue_size) {

    // empty_slots -> Semaphore counting empty slots in the queue
    // sem_open --> Create or open a named semaphore
    // ws_empty --> Name of the semaphore
    // O_CREAT --> Create the semaphore if it does not exist
    // 0666 --> Permissions for the semaphore -> rw-rw-rw- -> https://superuser.com/questions/295591/what-is-the-meaning-of-chmod-666
    // queue_size --> Initial value of the semaphore

    sems->empty_slots = sem_open("/ws_empty", O_CREAT, 0666, queue_size);

    // filled_slots -> Semaphore counting filled slots in the queue
    // ws_filled --> Name of the semaphore
    // 0 --> Initial value of the semaphore

    sems->filled_slots = sem_open("/ws_filled", O_CREAT, 0666, 0);
    
    // queue_mutex -> Mutex semaphore for protecting access to the queue
    // ws_queue_mutex --> Name of the semaphore
    // 1 --> Initial value of the semaphore -> binary semaphore 

    sems->queue_mutex = sem_open("/ws_queue_mutex", O_CREAT, 0666, 1);

    // stats_mutex -> Mutex semaphore for protecting access to statistics
    // ws_stats_mutex --> Name of the semaphore
    // 1 --> Initial value of the semaphore

    sems->stats_mutex = sem_open("/ws_stats_mutex", O_CREAT, 0666, 1);
    
    // log_mutex -> Mutex semaphore for protecting access to the log file
    // ws_log_mutex --> Name of the semaphore
    // 1 --> Initial value of the semaphore

    sems->log_mutex = sem_open("/ws_log_mutex", O_CREAT, 0666, 1);


    // Check if any semaphore failed to initialize
    // SEM_FAILED --> Constant indicating semaphore initialization failure

    if (sems->empty_slots == SEM_FAILED || sems->filled_slots == SEM_FAILED || sems->queue_mutex == SEM_FAILED || sems->stats_mutex == SEM_FAILED || sems->log_mutex == SEM_FAILED) {
        return -1;
    }

    return 0; // Success
}


// Function to destroy semaphores and clean up resources
// Arguments:
// sems - Pointer to the semaphores_t structure to be destroyed

void destroy_semaphores(semaphores_t* sems) {

    // Validate input parameter -> Check if the pointer is NULL
    if (!sems){
        return;
    };

    // Close each semaphore
    // sem_close --> Close the named semaphore

    sem_close(sems->empty_slots);
    sem_close(sems->filled_slots);
    sem_close(sems->queue_mutex);
    sem_close(sems->stats_mutex);
    sem_close(sems->log_mutex);

    // Unlink each semaphore
    // sem_unlink --> Remove the named semaphore
    
    sem_unlink("/ws_empty");
    sem_unlink("/ws_filled");
    sem_unlink("/ws_queue_mutex");
    sem_unlink("/ws_stats_mutex");
    sem_unlink("/ws_log_mutex");
}
