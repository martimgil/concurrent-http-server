#ifndef CONFIG_H
#define CONFIG_H

// Configuration structure for the server
typedef struct {
    
    int port; // Port number the server listens on
    char document_root[256]; // Root directory for serving files
    int num_workers; // Number of worker processes
    int threads_per_worker; // Number of threads per worker process
    int max_queue_size; // Maximum size of the request queue
    char log_file[256]; // Path to the log file
    int cache_size_mb; // Cache size in megabytes
    int timeout_seconds; // Timeout duration in seconds

} server_config_t; // Server configuration structure

// Function to load configuration from a file
int load_config(const char* filename, server_config_t* config);

#endif