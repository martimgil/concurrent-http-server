#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


// Function to load server configuration from a file
// Arguments:
// filename -> Path to the configuration file
// server_config_t* config -> Pointer to the server configuration structure to be filled

int load_config(const char* filename, server_config_t* config) {

    // Open the configuration file for reading
    FILE* fp = fopen(filename, "r");

    // Check if the file was opened successfully
    if (!fp) {
        return -1;
    }

    // Read each line and parse key-value pairs
    char line[512], key[128], value[256]; // Buffers for reading lines and storing keys/values

    // Loop through each line of the file
    // fgets -> Read a line from the file
    // sizeof(line) -> Maximum number of characters to read
    // fp -> File pointer to the configuration file

    while (fgets(line, sizeof(line), fp)) {

        // Skip comments and empty lines
        if (line[0] == '#' || line[0] == '\n') {
            continue;
        }

        // Parse key-value pairs in the format KEY=VALUE
        // sscanf -> Read formatted data from the string
        // "%127[^=]=%255s" -> Format specifier to read key and value
        // key -> Buffer to store the key
        // value -> Buffer to store the value
        // == 2 -> Expecting to read exactly 2 items (key and value)

        if (sscanf(line, "%127[^=]=%255s", key, value) == 2) {

            // Remove trailing whitespace from key
            char* p = key + strlen(key) - 1;

            // Loop backwards through the key string to trim spaces and tabs
            while (p > key && (*p == ' ' || *p == '\t')) *p-- = '\0';

            // Match keys and set corresponding configuration values

            if (strcmp(key, "PORT") == 0) { 
                // Convert the port value from string to integer
                // config->port -> Port number in the configuration structure
                // atoi -> Convert string to integer
            
                config->port = atoi(value);

            } else if (strcmp(key, "NUM_WORKERS") == 0) {

                // Convert the number of workers from string to integer
                // config->num_workers -> Number of worker processes in the configuration structure
                config->num_workers = atoi(value);

            } else if (strcmp(key, "THREADS_PER_WORKER") == 0) {

                // Convert the number of threads per worker from string to integer
                // config->threads_per_worker -> Number of threads per worker in the configuration structure

                config->threads_per_worker = atoi(value);

            } else if (strcmp(key, "DOCUMENT_ROOT") == 0) {

                // Copy the document root path into the configuration structure
                // config->document_root -> Document root path in the configuration structure
                // memcpy -> Copy the string with size limit
                size_t len = strlen(value);
                if (len > sizeof(config->document_root) - 1) len = sizeof(config->document_root) - 1;
                memcpy(config->document_root, value, len);
                config->document_root[len] = '\0';

            } else if (strcmp(key, "LOG_FILE") == 0) {

                // Copy the log file path into the configuration structure
                // config->log_file -> Log file path in the configuration structure
                size_t len = strlen(value);
                if (len > sizeof(config->log_file) - 1) len = sizeof(config->log_file) - 1;
                memcpy(config->log_file, value, len);
                config->log_file[len] = '\0';

            } else if (strcmp(key, "MAX_QUEUE_SIZE") == 0) {

                // Convert the max queue size from string to integer
                config->max_queue_size = atoi(value);

            } else if (strcmp(key, "CACHE_SIZE_MB") == 0) {

                // Convert the cache size from string to integer
                config->cache_size_mb = atoi(value);

            } else if (strcmp(key, "TIMEOUT_SECONDS") == 0) {

                // Convert the timeout duration from string to integer
                config->timeout_seconds = atoi(value);
            }
        }
    }

    // Close the configuration file
    fclose(fp);

    return 0;
}

