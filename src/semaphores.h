#ifndef SEMAPHORES_H
#define SEMAPHORES_H
#include <semaphore.h>
typedef struct {
    sem_t* empty_slots;
    sem_t* filled_slots;
    sem_t* queue_mutex;
    sem_t* stats_mutex;
    sem_t* log_mutex;
} semaphores_t;
int init_semaphores(semaphores_t* sems, int queue_size);
void destroy_semaphores(semaphores_t* sems);
#endif
