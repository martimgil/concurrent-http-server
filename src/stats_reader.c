#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include "shared_mem.h"

// Simple utility to read and print server statistics from shared memory
// This allows test scripts to verify statistics accuracy

int main(void) {
    // Open the shared memory object (same name as used by the server)
    int shm_fd = shm_open("/webserver_shm", O_RDONLY, 0666);
    
    if (shm_fd == -1) {
        fprintf(stderr, "Error: Could not open shared memory. Is the server running?\n");
        return 1;
    }
    
    // Map the shared memory into this process's address space
    shared_data_t* shm = mmap(NULL, sizeof(shared_data_t), PROT_READ, MAP_SHARED, shm_fd, 0);
    
    if (shm == MAP_FAILED) {
        fprintf(stderr, "Error: Could not map shared memory\n");
        close(shm_fd);
        return 1;
    }
    
    // Read and print statistics in a parseable format
    printf("total_requests=%ld\n", shm->stats.total_requests);
    printf("bytes_transferred=%ld\n", shm->stats.bytes_transferred);
    printf("status_200=%ld\n", shm->stats.status_200);
    printf("status_404=%ld\n", shm->stats.status_404);
    printf("status_500=%ld\n", shm->stats.status_500);
    printf("active_connections=%d\n", shm->stats.active_connections);
    printf("total_response_time_ms=%ld\n", shm->stats.total_response_time_ms);
    
    // Calculate average response time if there are requests
    if (shm->stats.total_requests > 0) {
        long avg_response_time = shm->stats.total_response_time_ms / shm->stats.total_requests;
        printf("avg_response_time_ms=%ld\n", avg_response_time);
    } else {
        printf("avg_response_time_ms=0\n");
    }
    
    // Clean up
    munmap(shm, sizeof(shared_data_t));
    close(shm_fd);
    
    return 0;
}
