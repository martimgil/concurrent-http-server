#ifndef TRABALHO2_SO_WORKER_H
#define TRABALHO2_SO_WORKER_H
#include "shared_mem.h"
#include "semaphores.h"

void worker_main(shared_data_t* shm, semaphores_t* sems, int worker_id, int channel_fd);
#endif 