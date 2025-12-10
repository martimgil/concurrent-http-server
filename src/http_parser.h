#ifndef HTTP_PARSER_H
#define HTTP_PARSER_H

#include <stddef.h>

// Structure to hold parsed HTTP request components
typedef struct {
    char method[16];   // HTTP method 
    char path[512];    // Request path 
    char version[16];  // HTTP version
    char range[64];    // Range header value, empty if not present
} http_request_t;

// Function to parse an HTTP request from a buffer
// Returns 0 on success, -1 on error
int parse_http_request(const char* buffer, http_request_t* req);

#endif 
