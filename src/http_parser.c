#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include <ctype.h>
#include "http_parser.h"

// Helper to trim whitespace
static char* trim_whitespace(char* str) {
    char* end; // Declare a pointer to mark the end of the non-whitespace content

    // Trim leading whitespace:
    // Iterate from the beginning of the string, incrementing 'str'
    // until a non-whitespace character is found.
    while (isspace((unsigned char)*str)) {
        str++;
    }

    // If the string is now empty (all whitespace or originally empty),
    // return it as is.
    if (*str == 0) {
        return str;
    }

    // Trim trailing whitespace:
    // Initialize 'end' to point to the last character of the string.
    end = str + strlen(str) - 1;

    // Iterate backwards from 'end' towards 'str'
    // until a non-whitespace character is found or 'end' crosses 'str'.
    while (end > str && isspace((unsigned char)*end)) {
        end--;
    }

    // Null-terminate the string right after the last non-whitespace character.
    // This effectively "trims" the trailing whitespace.
    *(end + 1) = 0;

    // Return the pointer to the start of the trimmed string.
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
    
    size_t header_len; // Length of the header

    // Calculate header length
    if (header_end) {
        header_len = (size_t)(header_end - buffer);
    } else {
        header_len = strlen(buffer);
    }

    // Limit header parsing to a reasonable size to prevent stack overflow
    if (header_len > 8192){
        header_len = 8192;
    }

    char local_buf[8193]; // Buffer for header parsing
    strncpy(local_buf, buffer, header_len); // Copy header to local buffer
    local_buf[header_len] = '\0'; // Null-terminate the buffer

    // Parse Request Line
    char* saveptr;
    char* line = strtok_r(local_buf, "\r\n", &saveptr);

    // Check if request line is valid
    if (!line){
        return -1; // Invalid request line
    }

    // Parse request line
    if (sscanf(line, "%15s %511s %15s", req->method, req->path, req->version) != 3) {
        return -1;
    }

    // Parse Headers
    while ((line = strtok_r(NULL, "\r\n", &saveptr))) {
        // Check for Range header
        // Case-insensitive check for "Range:"
        if (strncasecmp(line, "Range:", 6) == 0) {
            char* value = line + 6;  // Get range value
            value = trim_whitespace(value); // Trim whitespace from range value
            strncpy(req->range, value, sizeof(req->range) - 1); // Copy range value to request structure
        }
    }

    return 0;
}
