#include "config.h"
#include <stdio.h>
#include <string.h>


// Load configuration from a file (port, num workers, etc.) and parses them

int load_config(const char* filename, server_config_t* config) {
    FILE* fp = fopen(filename, "r");
    if (!fp) return -1;

    char line[512], key[128], value[256];
    while (fgets(line, sizeof(line), fp)) {
        if (line[0] == '#' || line[0] == '\n') continue;

        if (sscanf(line, "%[^=]=%s", key, value) == 2) {
            if (strcmp(key, "PORT") == 0) config->port = atoi(value);
            else if (strcmp(key, "NUM_WORKERS") == 0) config->num_workers =
           atoi(value);
            else if (strcmp(key, "THREADS_PER_WORKER") == 0)
                config->threads_per_worker = atoi(value);
            else if (strcmp(key, "DOCUMENT_ROOT") == 0)
                strncpy(config->document_root, value, sizeof(config-
               >document_root));
            // Add other config parameters...
        }
    }
    fclose(fp);
    return 0;
}