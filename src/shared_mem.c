#include "shared_mem.h"
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

// A função cria e inializa um segmento de memoria compartilhada POSIX
// Vai alocar memória na região para ser acessada por todos os processos do sistema

#define SHM_NAME "/webserver_shm"
shared_data_t* create_shared_memory() {
    int shm_fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (shm_fd == -1) return NULL;

    if (ftruncate(shm_fd, sizeof(shared_data_t)) == -1) {
        close(shm_fd);
        return NULL;
    }

    shared_data_t* data = mmap(NULL, sizeof(shared_data_t),
    PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    close(shm_fd);

    if (data == MAP_FAILED) return NULL;

    memset(data, 0, sizeof(shared_data_t));
    return data;
}

void destroy_shared_memory(shared_data_t* data) {
    if (data != NULL && data != MAP_FAILED) {
        munmap(data, sizeof(shared_data_t));
    }
    shm_unlink(SHM_NAME);
}