#include <sys/socket.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <time.h>

// Forward declaration for send_http_response_with_body_flag
void send_http_response_with_body_flag(int fd, int status, const char* status_msg, const char* content_type, const char* body, size_t body_len, int send_body);

// Function to send an HTTP response
// Arguments:
// fd - File descriptor to send the response to
// status - HTTP status code (e.g., 200, 404)
// status_msg - Status message corresponding to the status code (e.g., "OK", "Not Found")
// content_type - MIME type of the response body 
// body - Pointer to the response body
// body_len - Length of the response body in bytes
// send_body - If 0 (false), only sends headers (for HEAD requests), otherwise sends both headers and body

void send_http_response(int fd, int status, const char* status_msg, const char* content_type, const char* body, size_t body_len) {
    send_http_response_with_body_flag(fd, status, status_msg, content_type, body, body_len, 1);
}

// Internal function that supports body flag for HEAD requests
void send_http_response_with_body_flag(int fd, int status, const char* status_msg, const char* content_type, const char* body, size_t body_len, int send_body) {

    // Validate input parameters
    // Ensure file descriptor is valid and required strings are not NULL
    // !status_msg --> Check if status message is provided
    // !content_type --> Check if content type is provided

    if (fd < 0 || !status_msg || !content_type) {
        return;
    }

    time_t now = time(NULL); // Get the current time
    struct tm tm = *gmtime(&now); // Convert to GMT time structure
    char date_str[64]; // Buffer to hold formatted date string

    // Format the date string according to HTTP specifications
    // strftime --> Format date and time
    strftime(date_str, sizeof(date_str), "%a, %d %b %Y %H:%M:%S GMT", &tm);



    // Construct HTTP response header
    char header[2048];

    // Format the HTTP response header
    // header_len --> Length of the formatted header
    // snprintf --> Format the header string with status, content type, and content length
    
    int header_len = snprintf(header, sizeof(header),
    "HTTP/1.1 %d %s\r\n" // Status line
    "Content-Type: %s\r\n" // Content-Type header
    "Content-Length: %zu\r\n" // Content-Length header
    "Server: ConcurrentHTTP/1.0\r\n" // Server header
    "Date: %s\r\n" // Date header
    "Connection: close\r\n" // Connection header
    "\r\n",
    status, status_msg, content_type, body_len, date_str); // Get current date string


    // Check for formatting errors
    // header_len < 0 --> Error during formatting
    // header_len >= sizeof(header) --> Header truncated

    if (header_len < 0 || header_len >= (int)sizeof(header)) {
        perror("Header formatting failed");
        return;
    }

    // Send headers with loop to handle partial sends (FAQ Q15)
    // ssize_t -> Signed size type for number of bytes sent
    // send --> Send data over the socket
    ssize_t total_sent = 0;
    while (total_sent < header_len) {
        ssize_t sent = send(fd, header + total_sent, header_len - total_sent, 0);
        
        if (sent < 0) {
            perror("Failed to send header");
            return;
        }
        
        if (sent == 0) {
            // Connection closed
            return;
        }
        
        total_sent += sent;
    }

    // Send the response body if provided and if send_body flag is set
    // body --> Pointer to the response body
    // body_len --> Length of the response body in bytes
    // send_body --> Flag indicating if body should be sent (0 for HEAD requests, 1 for GET)

    if (send_body && body && body_len > 0) {

        // Send the body with loop to handle partial sends (FAQ Q15)
        total_sent = 0;
        while (total_sent < (ssize_t)body_len) {
            ssize_t sent = send(fd, body + total_sent, body_len - total_sent, 0);
            
            if (sent < 0) {
                perror("Failed to send body");
                return;
            }
            
            if (sent == 0) {
                // Connection closed
                return;
            }
            
            total_sent += sent;
        }
    }
}
