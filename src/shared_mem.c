#include "shared_mem.h"
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

// Name of the shared memory object
// webserver_shm -> Shared memory for the web server
#define SHM_NAME "/webserver_shm"

// Function to create and initialize shared memory
shared_data_t* __attribute__((no_sanitize("thread"))) create_shared_memory(int queue_size __attribute__((unused))) {

    // shm_fd -> File descriptor for the shared memory object
    // shm_open -> Create or open a shared memory object
    // O_CREAT | O_RDWR -> Create if it doesn't exist, open for reading and writing
    // 0666 -> Permissions for the shared memory -> rw-rw-rw- -> https://superuser.com/questions/295591/what-is-the-meaning-of-chmod-666

    int shm_fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);

    // Check for errors in creating/opening shared memory
    if (shm_fd == -1){
        return NULL;
    };

    // Set the size of the shared memory object
    // ftruncate -> Set the size of the shared memory object
    // sizeof(shared_data_t) -> Size of the shared data structure
    // == -1 -> Check for errors
    if (ftruncate(shm_fd, sizeof(shared_data_t)) == -1) {

        // Close the shared memory file descriptor on error
        close(shm_fd);
        return NULL;

    }

    // Map the shared memory object into the process's address space
    // data -> Pointer to the mapped shared memory region
    // mmap -> Map the shared memory object
    // NULL -> Let the system choose the address
    // sizeof(shared_data_t) -> Size of the mapping
    // PROT_READ | PROT_WRITE -> Read and write permissions
    // MAP_SHARED -> Shared mapping

    shared_data_t* data = mmap(NULL, sizeof(shared_data_t), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);

    close(shm_fd); // Close the file descriptor as it's no longer needed


    // Check for errors in mapping shared memory
    // data == MAP_FAILED -> Check if the mapping failed
    if (data == MAP_FAILED){
        return NULL;
    }

    // Initialize shared data to zero
    // memset -> Set memory to a specific value
    // data -> Pointer to the shared data structure
    // 0 -> Value to set (zero)
    // sizeof(shared_data_t) -> Size of the memory to set
    memset(data, 0, sizeof(shared_data_t));

    return data; // Return pointer to the shared data structure
}

// Function to destroy shared memory
void __attribute__((no_sanitize("thread"))) destroy_shared_memory(shared_data_t* data) {

    // Unmap the shared memory region and unlink the shared memory object
    if (data != NULL && data != MAP_FAILED) {
        // munmap -> Unmap the shared memory region
        // data -> Pointer to the mapped shared memory region
        // sizeof(shared_data_t) -> Size of the mapping

        munmap(data, sizeof(shared_data_t));
    }
    
    // shm_unlink -> Remove the shared memory object
    shm_unlink(SHM_NAME);
}