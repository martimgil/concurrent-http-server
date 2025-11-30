#define _GNU_SOURCE
#include "logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include <errno.h>

// ###################################################################################################################
// ESTADO INTERNO DO LOGGER
// ###################################################################################################################

// Caminho absoluto do ficheiro de log
static char g_log_path[512];

// File descriptor usado para escrita
static int g_log_fd = -1;

// Semáforo POSIX global para garantir exclusão mútua entre processos & threads
static sem_t* g_sem = NULL;

// Nome fixo do semáforo POSIX (deve começar com '/')
static const char* LOG_SEM_NAME = "/ws_log_sem";

// Tamanho máximo permitido antes da rotação
static const off_t LOG_MAX_SIZE = 10 * 1024 * 1024;  // 10 MB

// Quantidade de ficheiros de histórico
static const int LOG_MAX_ROTATIONS = 5;


// ###################################################################################################################
// Função interna robusta: escrever todos os bytes (tratando erros e partial write)
// ###################################################################################################################
static int write_all(int fd, const void* buf, size_t len)
{
    const char* p = (const char*)buf;
    size_t off = 0;

    while (off < len) {
        ssize_t n = write(fd, p + off, len - off);

        if (n > 0) {
            off += (size_t)n;
            continue;
        }

        if (n < 0 && errno == EINTR)
            continue;  // tentar outra vez se foi interrompido

        return -1; // erro permanente
    }

    return 0; // sucesso total
}


// ###################################################################################################################
// Função interna: obter tamanho do ficheiro actual
// ###################################################################################################################
static off_t get_file_size(int fd){
    struct stat st;
    if (fstat(fd, &st) == 0) return st.st_size;
    return -1;
}


// ###################################################################################################################
// Função interna: efetuar rotação automática do log
// access.log → access.log.1 → ... → access.log.5
// ###################################################################################################################
static void rotate_logs(){

    // Fechar ficheiro atual antes de rodar
    if (g_log_fd >= 0){
        close(g_log_fd);
        g_log_fd = -1;
    }

    // Apagar o ficheiro mais antigo
    char oldpath[600], newpath[600];

    snprintf(oldpath, sizeof(oldpath), "%s.%d", g_log_path, LOG_MAX_ROTATIONS);
    unlink(oldpath);

    // Rodar do N-1 até 1
    for (int i = LOG_MAX_ROTATIONS - 1; i >= 1; i--){
        snprintf(oldpath, sizeof(oldpath), "%s.%d", g_log_path, i);
        snprintf(newpath, sizeof(newpath), "%s.%d", g_log_path, i + 1);
        rename(oldpath, newpath);
    }

    // Mover o principal → .1
    snprintf(newpath, sizeof(newpath), "%s.1", g_log_path);
    rename(g_log_path, newpath);

    // Reabrir o principal vazio
    g_log_fd = open(g_log_path, O_CREAT | O_WRONLY | O_APPEND, 0644);
}


// ###################################################################################################################
// Inicialização do logger
// ###################################################################################################################
void logger_init(const char* logfile_path){

    // Guardar caminho
    strncpy(g_log_path, logfile_path, sizeof(g_log_path)-1);
    g_log_path[sizeof(g_log_path)-1] = '\0';

    // Criar/abrir semáforo POSIX partilhado entre processos
    g_sem = sem_open(LOG_SEM_NAME, O_CREAT, 0666, 1);

    if (g_sem == SEM_FAILED){
        perror("logger: sem_open");
        exit(1);
    }

    // Abrir ficheiro de log
    g_log_fd = open(g_log_path, O_CREAT | O_WRONLY | O_APPEND, 0644);

    if (g_log_fd < 0){
        perror("logger: open logfile");
        exit(1);
    }
}


// ###################################################################################################################
// Encerrar logger
// ###################################################################################################################
void logger_close(){
    if (g_log_fd >= 0){
        close(g_log_fd);
        g_log_fd = -1;
    }

    if (g_sem){
        sem_close(g_sem);
        // NOTA: não fazemos sem_unlink aqui, para evitar condições no shutdown
    }
}


// ###################################################################################################################
// Escreve uma entrada no log (com semáforo + rotação automática)
// ###################################################################################################################
void logger_write(const char* ip,
                  const char* method,
                  const char* path,
                  int status,
                  size_t bytes_sent,
                  long duration_ms)
{
    if (!g_sem || g_log_fd < 0)
        return;

    // Entrar na secção crítica (bloqueia processos + threads)
    sem_wait(g_sem);

    // Verificar se excedemos o tamanho máximo → rotacionar
    off_t size_now = get_file_size(g_log_fd);
    if (size_now >= LOG_MAX_SIZE){
        rotate_logs();
    }

    // Obter timestamp formatado
    char datebuf[64];
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    strftime(datebuf, sizeof(datebuf), "%d/%b/%Y:%H:%M:%S", &tm_now);

    // Formatar a linha
    char line[1024];
    int len = snprintf(
        line, sizeof(line),
        "%s [%s] \"%s %s\" %d %zu %ldms\n",
        ip, datebuf, method, path, status, bytes_sent, duration_ms
    );

    if (len > 0){
        if (write_all(g_log_fd, line, (size_t)len) != 0) {
            int e = errno;
            dprintf(STDERR_FILENO, "logger: write failed: %s\n", strerror(e));
        }
    }

    // Sair da secção crítica
    sem_post(g_sem);
}
