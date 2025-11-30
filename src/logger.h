#ifndef LOGGER_H
#define LOGGER_H

#include <semaphore.h>
#include <stddef.h>

// ###################################################################################################################
// Thread-Safe & Process-Safe Logger (Feature 5)
// 
// Este módulo implementa um sistema de logging seguro, partilhado entre múltiplos processos
// (master + workers) e múltiplas threads (cada worker).
//
// A escrita no ficheiro de log é protegida por:
//   - Um semáforo POSIX nomeado (sem_open)
//   - Abertura com O_APPEND (append seguro atómico)
//
// Inclui também rotação automática do ficheiro de log quando excede 10 MB.
// ###################################################################################################################

void logger_init(const char* logfile_path);    // Inicializa logger (abre semáforo + ficheiro)
void logger_close();                           // Fecha recursos (sem_close etc.)
void logger_write(const char* ip,
                  const char* method,
                  const char* path,
                  int status,
                  size_t bytes_sent,
                  long duration_ms);           // Escreve entrada no log (segura e rotacionada)

#endif
