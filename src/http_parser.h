#ifndef HTTP_PARSER_H
#define HTTP_PARSER_H

#include <stddef.h>

// Structure to hold parsed HTTP request components
typedef struct {
    char method[16];   // HTTP method (e.g., GET, POST)
    char path[512];    // Request path (e.g., /index.html)
    char version[16];  // HTTP version (e.g., HTTP/1.1)
    char range[64];    // Range header value (e.g., "bytes=0-1023"), empty if not present
} http_request_t;

// Function to parse an HTTP request from a buffer
// Returns 0 on success, -1 on error
int parse_http_request(const char* buffer, http_request_t* req);

#endif /* HTTP_PARSER_H */
