#ifndef HTTP_BUILDER_H
#define HTTP_BUILDER_H

#include <stddef.h> // size_t

// Sends an HTTP response.
// The 'keep_alive' parameter (1 or 0) sets the header to "Connection: keep-alive" or "close".
void send_http_response(int fd, int status, const char* status_msg, 
                        const char* content_type, const char* body, size_t body_len, 
                        int keep_alive);

// Internal version with a flag to control body sending (useful for HEAD requests)
void send_http_response_with_body_flag(int fd, int status, const char* status_msg, 
                                       const char* content_type, const char* body, size_t body_len, 
                                       int send_body, int keep_alive);

// Sends an HTTP 206 Partial Content response.
void send_http_partial_response(int fd, const char* content_type, const char* body, size_t body_len, 
                                size_t start, size_t end, size_t total_size, int keep_alive);

// Sends an nginx-style error page response (400, 403, 404, 405, 416, 500, 503, etc.)
void send_error_response(int fd, int status, const char* status_msg, int keep_alive);

#endif