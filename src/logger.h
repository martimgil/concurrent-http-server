#ifndef LOGGER_H
#define LOGGER_H

#include <semaphore.h>
#include <stddef.h>

// ###############################################################################################################
// Thread-Safe & Process-Safe Logger (Feature 5)
//
// This module implements a logging system that is safe for use by multiple processes (master + workers)
// and multiple threads (within each worker).
//
// Log file writing is protected by:
//   - A named POSIX semaphore (sem_open) for inter-process synchronization
//   - File opened with O_APPEND for atomic append operations
//
// The logger also supports automatic log rotation when the file exceeds 10 MB.
// ###############################################################################################################

/**
 * Initializes the logger.
 * Opens the log file and the named semaphore for synchronization.
 */
void logger_init(const char* logfile_path);

/**
 * Closes the logger.
 * Releases all resources (closes file and semaphore).
 */
void logger_close();

/**
 * Writes a log entry in a thread-safe and process-safe manner.
 * Automatically rotates the log file if it exceeds 10 MB.
 */
void logger_write(const char* ip, // Client IP address
                  const char* method, // HTTP method
                  const char* path, // Request path
                  int status, // HTTP status code
                  size_t bytes_sent, // Number of bytes sent
                  long duration_ms); // Request duration in milliseconds  
                  

#endif // LOGGER_H
