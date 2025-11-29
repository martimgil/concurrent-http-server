#ifndef TRABALHO2_SO_LOGGER_H
#define TRABALHO2_SO_LOGGER_H

#include <semaphore.h>
#include <stddef.h>

// Function to log an HTTP request to the access log file
// Arguments:
// log_sem - Semaphore for synchronizing access to the log file
// client_ip - IP address of the client making the request
// method - HTTP method used in the request (e.g., GET, POST)
// path - Requested resource path
// status - HTTP status code of the response
// bytes - Number of bytes sent in the response
void log_request(sem_t* log_sem, const char* client_ip, const char* method, const char* path, int status, size_t bytes);

#endif 