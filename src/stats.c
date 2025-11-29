#include "stats.h"
#include <stdio.h>
#include <sys/time.h>

long get_time_ms(){
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (tv.tv_sec * 1000) + (tv.tv_usec/1000);
}

void update_stats(shared_data_t* shm, semaphores_t* sems, int status_code, long bytes, long time_taken_ms){


    // Lock the stats mutex
    sem_wait(sems->stats_mutex);
    
    // Update total bytes transferred
    shm->stats.total_requests++; // Increment total requests
    shm->stats.bytes_transferred += bytes; // Add bytes transferred
    shm->stats.total_response_time_ms += time_taken_ms; // Add response time


    // Update status code counts
    if(status_code == 200){ // HTTP 200 OK
        shm->stats.status_200++; // Increment 200 count
    } else if(status_code == 404){ // HTTP 404 Not Found
        shm->stats.status_404++; // Increment 404 count
    } else if(status_code == 500){ // HTTP 500 Internal Server Error
        shm->stats.status_500++; // Increment 500 count
    }

    // Unlock the stats mutex
    sem_post(sems->stats_mutex); // Release the lock

}

void print_stats(shared_data_t* shm, semaphores_t* sems){

    sem_wait(sems->stats_mutex); // Lock the stats mutex

    // Calculate average response time
    double avg_time = 0; // Initialize average time

    // Check if total requests is greater than 0
    if(shm -> stats.total_requests >0) {
        // Calculate average response time
        avg_time = (double)shm->stats.total_response_time_ms / shm->stats.total_requests;
    }

    printf("\n--- Server Statistics ---\n");
    printf("Total Requests: %ld\n", shm->stats.total_requests);
    printf("Bytes Transferred: %ld\n", shm->stats.bytes_transferred);
    printf("Average Response Time: %.2f ms\n", avg_time);
    printf("Status Code: [200: %ld] [404: %ld] [500: %ld]\n",
           shm->stats.status_200,
           shm->stats.status_404,
           shm->stats.status_500);
    printf("-------------------------\n");

    sem_post(sems->stats_mutex); // Release the lock

}