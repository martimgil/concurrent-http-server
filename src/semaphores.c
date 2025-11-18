#include "semaphores.h"
#include <fcntl.h>

// Inicializa e dentroi POSIX semaforos usados na sincronização com um servidor multi-theread
int init_semaphores(semaphores_t* sems, int queue_size) {
    sems->empty_slots = sem_open("/ws_empty", O_CREAT, 0666, queue_size);
    sems->filled_slots = sem_open("/ws_filled", O_CREAT, 0666, 0);
    sems->queue_mutex = sem_open("/ws_queue_mutex", O_CREAT, 0666, 1);
    sems->stats_mutex = sem_open("/ws_stats_mutex", O_CREAT, 0666, 1);
    sems->log_mutex = sem_open("/ws_log_mutex", O_CREAT, 0666, 1);

    if (sems->empty_slots == SEM_FAILED || sems->filled_slots == SEM_FAILED || sems->queue_mutex == SEM_FAILED || sems->stats_mutex == SEM_FAILED || sems->log_mutex == SEM_FAILED) {
        return -1;
    }
    return 0;
}

void destroy_semaphores(semaphores_t* sems) {
    if (!sems) return;

    sem_close(sems->empty_slots);
    sem_close(sems->filled_slots);
    sem_close(sems->queue_mutex);
    sem_close(sems->stats_mutex);
    sem_close(sems->log_mutex);

    sem_unlink("/ws_empty");
    sem_unlink("/ws_filled");
    sem_unlink("/ws_queue_mutex");
    sem_unlink("/ws_stats_mutex");
    sem_unlink("/ws_log_mutex");
}
