#ifndef STATS_H
#define STATS_H

#include "shared_mem.h"
#include "semaphores.h"
#include <time.h>

// Update statistics based on a completed request
// Arguments:
// shm - Pointer to the shared memory structure
// sems - Pointer to the semaphores structure
// status_code - HTTP status code of the completed request
// bytes - Number of bytes transferred in the request
// time_taken_ms - Time taken to process the request in milliseconds
void update_stats(shared_data_t* shm, semaphores_t* sems, int status_code, long bytes, long time_taken_ms);

// Print current statistics to the console
// Arguments:
// shm - Pointer to the shared memory structure
// sems - Pointer to the semaphores structure
void print_stats(shared_data_t* shm, semaphores_t* sems);

// Get current time in milliseconds
long get_time_ms();

#endif