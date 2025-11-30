#include <stdio.h>
#include <string.h>
#include <stddef.h>
// Structure to hold parsed HTTP request components
typedef struct {
    char method[16]; // HTTP method (e.g., GET, POST)
    char path[512];  // Request path (e.g., /index.html)
    char version[16]; // HTTP version (e.g., HTTP/1.1)
} http_request_t; // HTTP request structure


// Function to parse an HTTP request from a buffer
// Arguments:
// buffer - Pointer to the buffer containing the HTTP request
// req - Pointer to the http_request_t structure to store parsed components

int parse_http_request(const char* buffer, http_request_t* req) {

    // Validate input parameters
    if (!buffer || !req){
        return -1;
    }

    // Extract the first line of the HTTP request
    // Find the end of the first line
    // line_end --> Pointer to the end of the first line
    // strstr --> Locate the first occurrence of "\r\n" in the buffer

    char* line_end = strstr(buffer, "\r\n");
    
    // Check if the first line was found
    if (!line_end){
        return -1;
    }

    // Copy the first line into a temporary buffer
    // first_line --> Temporary buffer to hold the first line
    char first_line[1024];

    // Calculate the length of the first line
    // len --> Length of the first line
    // line_end -> Pointer to the end of the first line
    // buffer -> Pointer to the start of the buffer
    // line_end - buffer --> Length of the first line
    size_t len = line_end - buffer;

    // Ensure the length does not exceed the size of first_line
    if (len >= sizeof(first_line)){ 
        return -1;
    }

    // Copy the first line into first_line
    // strncpy --> Copy the first line into first_line
    // first_line --> Temporary buffer to hold the first line
    // buffer --> Pointer to the start of the buffer
    // len --> Length of the first line

    strncpy(first_line, buffer, len);
    
    first_line[len] = '\0'; // Null-terminate the string

    // Use sscanf with width limits to prevent buffer overflows
    // sscanf --> Read formatted data from the string
    if (sscanf(first_line, "%15s %511s %15s", req->method, req->path, req->version) != 3) {
        return -1;
    }

    return 0;
}
