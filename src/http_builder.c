#include <sys/socket.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include "http_builder.h"

// Forward declaration for send_http_response_with_body_flag
void send_http_response_with_body_flag(int fd, int status, const char* status_msg, const char* content_type, const char* body, size_t body_len, int send_body, int keep_alive);

// Generate nginx-style error page HTML
// Returns the length of the generated HTML
static int generate_error_page(char* buffer, size_t buffer_size, int status, const char* status_msg) {
    return snprintf(buffer, buffer_size,
        "<!DOCTYPE html>\n"
        "<html>\n"
        "<head>\n"
        "<title>%d %s</title>\n"
        "<style>\n"
        "    body {\n"
        "        font-family: Tahoma, Verdana, Arial, sans-serif;\n"
        "        background-color: #fff;\n"
        "        color: #000;\n"
        "        margin: 0;\n"
        "        padding: 0;\n"
        "    }\n"
        "    .container {\n"
        "        width: 100%%;\n"
        "        margin: 0 auto;\n"
        "        text-align: center;\n"
        "        padding-top: 10%%;\n"
        "    }\n"
        "    h1 {\n"
        "        font-size: 36px;\n"
        "        font-weight: normal;\n"
        "        margin-bottom: 10px;\n"
        "    }\n"
        "    hr {\n"
        "        border: none;\n"
        "        border-top: 1px solid #ccc;\n"
        "        width: 50%%;\n"
        "        margin: 20px auto;\n"
        "    }\n"
        "    .footer {\n"
        "        font-size: 12px;\n"
        "        color: #333;\n"
        "    }\n"
        "</style>\n"
        "</head>\n"
        "<body>\n"
        "<div class=\"container\">\n"
        "    <h1>%d %s</h1>\n"
        "    <hr>\n"
        "    <p class=\"footer\">ConcurrentHTTP/1.0</p>\n"
        "</div>\n"
        "</body>\n"
        "</html>\n",
        status, status_msg, status, status_msg);
}

// Send an nginx-style error page response
void send_error_response(int fd, int status, const char* status_msg, int keep_alive) {
    char error_page[2048];
    int page_len = generate_error_page(error_page, sizeof(error_page), status, status_msg);
    if (page_len > 0 && page_len < (int)sizeof(error_page)) {
        send_http_response(fd, status, status_msg, "text/html; charset=utf-8", error_page, (size_t)page_len, keep_alive);
    }
}

// Function to send an HTTP response
// Arguments:
// fd - File descriptor to send the response to
// status - HTTP status code (e.g., 200, 404)
// status_msg - Status message corresponding to the status code (e.g., "OK", "Not Found")
// content_type - MIME type of the response body 
// body - Pointer to the response body
// body_len - Length of the response body in bytes
// keep_alive - If 1, sends "Connection: keep-alive", otherwise "Connection: close"

void send_http_response(int fd, int status, const char* status_msg, const char* content_type, const char* body, size_t body_len, int keep_alive) {
    send_http_response_with_body_flag(fd, status, status_msg, content_type, body, body_len, 1, keep_alive);
}

// Internal function that supports body flag for HEAD requests
void send_http_response_with_body_flag(int fd, int status, const char* status_msg, const char* content_type, const char* body, size_t body_len, int send_body, int keep_alive) {

    // Validate input parameters
    // Ensure file descriptor is valid and required strings are not NULL
    // !status_msg --> Check if status message is provided
    // !content_type --> Check if content type is provided

    if (fd < 0 || !status_msg || !content_type) {
        return;
    }

    time_t now = time(NULL); // Get the current time
    struct tm tm; // Time structure for thread-safe gmtime_r
    gmtime_r(&now, &tm); // Convert to GMT time structure (thread-safe)
    char date_str[64]; // Buffer to hold formatted date string

    // Format the date string according to HTTP specifications
    // strftime --> Format date and time
    strftime(date_str, sizeof(date_str), "%a, %d %b %Y %H:%M:%S GMT", &tm);

    // Determine Connection header value
    const char* connection_val;
    if (keep_alive) {
        connection_val = "keep-alive";
    } else {
        connection_val = "close";
    }

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
    "Connection: %s\r\n" // Connection header
    "\r\n",
    status, status_msg, content_type, body_len, date_str, connection_val); // Get current date string


    // Check for formatting errors
    // header_len < 0 --> Error during formatting
    // header_len >= sizeof(header) --> Header truncated

    if (header_len < 0 || header_len >= (int)sizeof(header)) {
        perror("Header formatting failed");
        return;
    }

    // Send headers with loop to handle partial sends
    // ssize_t -> Signed size type for number of bytes sent
    // send --> Send data over the socket
    ssize_t total_sent = 0;


    // Send headers with loop to handle partial sends
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

// Function to send an HTTP 206 Partial Content response
void send_http_partial_response(int fd, const char* content_type, const char* body, size_t body_len, 
                                size_t start, size_t end, size_t total_size, int keep_alive) {

    if (fd < 0 || !content_type) {
        return;
    }

    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    char date_str[64];
    strftime(date_str, sizeof(date_str), "%a, %d %b %Y %H:%M:%S GMT", &tm);

    const char* connection_val; // Connection header value

    // Set connection header value based on keep_alive flag
    if (keep_alive) {
        connection_val = "keep-alive";
    } else {
        connection_val = "close";
    }

    char header[2048]; // Buffer to hold formatted header

    // Format the HTTP response header
    int header_len = snprintf(header, sizeof(header),
    "HTTP/1.1 206 Partial Content\r\n"
    "Content-Type: %s\r\n"
    "Content-Length: %zu\r\n"
    "Content-Range: bytes %zu-%zu/%zu\r\n"
    "Server: ConcurrentHTTP/1.0\r\n"
    "Date: %s\r\n"
    "Connection: %s\r\n"
    "\r\n",
    content_type, body_len, start, end, total_size, date_str, connection_val);


    // Check for formatting errors
    if (header_len < 0 || header_len >= (int)sizeof(header)) {
        perror("Header formatting failed");
        return;
    }

    // Send headers
    ssize_t total_sent = 0;

    // Send headers with loop to handle partial sends
    while (total_sent < header_len) {

        // Send data over the socket
        ssize_t sent = send(fd, header + total_sent, header_len - total_sent, 0);
        
        if (sent <= 0){
            return;
        } // Error handling

        total_sent += sent; // Update total sent bytes
    }

    // Send body
    if (body && body_len > 0) {

        // Send body with loop to handle partial sends
        total_sent = 0;

        // Loop through the body and send it in chunks
        while (total_sent < (ssize_t)body_len) {

            // Send data over the socket
            ssize_t sent = send(fd, body + total_sent, body_len - total_sent, 0);
            
            if (sent <= 0){
                return;
            } // Error handling
            
            total_sent += sent; // Update total sent bytes
        }
    }
}
