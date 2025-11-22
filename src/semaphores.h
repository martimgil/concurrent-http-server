#ifndef SEMAPHORES_H
#define SEMAPHORES_H
#include <semaphore.h>

// Structure to hold semaphores used for synchronization
typedef struct {
    sem_t* empty_slots; // Semaphore counting empty slots in the queue
    sem_t* filled_slots; // Semaphore counting filled slots in the queue
    sem_t* queue_mutex; // Mutex semaphore for protecting access to the queue
    sem_t* stats_mutex; // Mutex semaphore for protecting access to statistics
    sem_t* log_mutex; // Mutex semaphore for protecting access to the log file
} semaphores_t; // Semaphores structure

// Function to initialize semaphores
// Arguments:
// sems - Pointer to the semaphores_t structure to be initialized
// queue_size - Size of the connection queue

int init_semaphores(semaphores_t* sems, int queue_size);

// Function to destroy semaphores and clean up resources
// Arguments:
// sems - Pointer to the semaphores_t structure to be destroyed
void destroy_semaphores(semaphores_t* sems);


#endif
