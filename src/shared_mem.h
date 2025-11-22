#ifndef SHARED_MEM_H
#define SHARED_MEM_H
#define MAX_QUEUE_SIZE 100

// Structure to hold server statistics in shared memory
typedef struct {
    long total_requests; // Total number of requests handled by the server
    long bytes_transferred; // Total bytes transferred by the server
    long status_200; // Count of HTTP 200 status responses
    long status_404; // Count of HTTP 404 status responses
    long status_500; // Count of HTTP 500 status responses
    int active_connections; // Number of active connections to the server
} server_stats_t; // Server statistics structure

// FEATURE 1: Bounded circular buffer in shared memory (size: 100 connections)
// Structure to hold the connection queue in shared memory
// Circular buffer to hold client socket file descriptors
typedef struct {
    int sockets[MAX_QUEUE_SIZE]; // Array to hold client socket file descriptors
    int front; // Index of the front of the queue
    int rear; // Index of the rear of the queue
    int count; // Number of elements in the queue
} connection_queue_t; // Connection queue structure

// Combined shared data structure
typedef struct {
    connection_queue_t queue; // Connection queue
    server_stats_t stats; // Server statistics
} shared_data_t; // Combined shared data structure

shared_data_t* create_shared_memory(); // Allocate and initialize the shared memory segment
    
void destroy_shared_memory(shared_data_t* data); // Clean up and release the shared memory

#endif