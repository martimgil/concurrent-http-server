#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include <ctype.h>
#include "http_parser.h"

// Helper to trim whitespace
static char* trim_whitespace(char* str) {
    char* end;
    while(isspace((unsigned char)*str)) str++;
    if(*str == 0) return str;
    end = str + strlen(str) - 1;
    while(end > str && isspace((unsigned char)*end)) end--;
    *(end+1) = 0;
    return str;
}

// Function to parse an HTTP request from a buffer
// Arguments:
// buffer - Pointer to the buffer containing the HTTP request
// req - Pointer to the http_request_t structure to store parsed components

int parse_http_request(const char* buffer, http_request_t* req) {

    // Validate input parameters
    if (!buffer || !req){
        return -1;
    }

    // Initialize the request structure
    memset(req, 0, sizeof(http_request_t));
    
    char* header_end = strstr(buffer, "\r\n\r\n"); // Find the end of headers
    size_t header_len;
    if (header_end) {
        header_len = (size_t)(header_end - buffer);
    } else {
        header_len = strlen(buffer);
    }
    // Limit header parsing to a reasonable size to prevent stack overflow
    if (header_len > 8192) header_len = 8192;

    char local_buf[8193]; // Buffer for header parsing
    strncpy(local_buf, buffer, header_len); // Copy header to local buffer
    local_buf[header_len] = '\0'; // Null-terminate the buffer

    // Parse Request Line
    char* line = strtok(local_buf, "\r\n");
    if (!line) return -1; // Invalid request line


    if (sscanf(line, "%15s %511s %15s", req->method, req->path, req->version) != 3) {
        return -1;
    }

    // Parse Headers
    while ((line = strtok(NULL, "\r\n"))) {
        // Check for Range header
        // Case-insensitive check for "Range:"
        if (strncasecmp(line, "Range:", 6) == 0) {
            char* value = line + 6;
            value = trim_whitespace(value);
            strncpy(req->range, value, sizeof(req->range) - 1);
        }
    }

    return 0;
}
