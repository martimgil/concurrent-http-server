#include <semaphore.h>
#include <stdio.h>
#include <time.h>
#include <stddef.h>
#include <string.h>

// Function to log an HTTP request to the access log file
// Arguments:
// log_sem - Semaphore for synchronizing access to the log file
// client_ip - IP address of the client making the request
// method - HTTP method used in the request (e.g., GET, POST)
// path - Requested resource path
// status - HTTP status code of the response
// bytes - Number of bytes sent in the response

void log_request(sem_t* log_sem, const char* client_ip, const char* method, const char* path, int status, size_t bytes) {

    // time_t -> Data type for time representation
    // now -> Current time
    // time(NULL) -> Get the current time
    time_t now = time(NULL);

    // struct tm -> Structure to hold broken-down time
    // tm_info -> Pointer to the local time structure
    // localtime(&now) -> Convert the time to local time
    struct tm* tm_info = localtime(&now);

    // char timestamp[64] -> Buffer to hold the formatted timestamp
    char timestamp[64];

    // strftime -> Format the time into a string
    // timestamp -> Buffer to store the formatted time
    // sizeof(timestamp) -> Size of the buffer
    strftime(timestamp, sizeof(timestamp), "%d/%b/%Y:%H:%M:%S %z", tm_info);

    // Log the request to the access log file
    // sem_wait -> Wait (decrement) the semaphore to gain access
    // log_sem -> Semaphore for synchronizing access to the log file

    sem_wait(log_sem);

    // FILE* log -> File pointer for the access log file
    // fopen -> Open the access log file for appending
    // a -> Append mode
    FILE* log = fopen("access.log", "a");
    
    // Check if the log file was opened successfully
    if (log) {
        // fprintf -> Write the formatted log entry to the file
        fprintf(log, "%s - - [%s] \"%s %s HTTP/1.1\" %d %zu\n", client_ip, timestamp, method, path, status, bytes);

        // fclose -> Close the log file
        fclose(log);
    }

    // sem_post -> Signal (increment) the semaphore to release access
    // log_sem -> Semaphore for synchronizing access to the log file
    sem_post(log_sem);
}

