#define _POSIX_C_SOURCE 200809L

#include "ipc.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

// Указатели наследуются при fork, адреса отображений остаются теми же
static sem_t **semaphore_slot(Simulation *simulation, int index) {
    if (index == 0) {
        return &simulation->mutex;
    }
    if (index == 1) {
        return &simulation->dispatcher_event;
    }
    if (index == 2) {
        return &simulation->start_event;
    }
    return &simulation->stop_event[index - 3];
}

static sem_t *create_semaphore(Simulation *simulation, int index) {
    unsigned int value = index == 0 ? 1 : 0;
#ifdef USE_UNNAMED_SEMAPHORES
    sem_t *semaphore = &simulation->semaphores[index];
    // pshared = 1: семафор в общей памяти используется разными процессами
    if (sem_init(semaphore, 1, value) != 0) {
        perror("sem_init");
        return NULL;
    }
#else
    (void)simulation;
    char name[64];
    snprintf(name, sizeof(name), "/water_%ld_%d", (long)getpid(), index);
    sem_t *semaphore = sem_open(name, O_CREAT | O_EXCL, 0600, value);
    if (semaphore == SEM_FAILED) {
        perror("sem_open");
        return NULL;
    }

    // убираем имя сразу, сам семафор остаётся доступен до sem_close
    if (sem_unlink(name) != 0) {
        perror("sem_unlink");
        sem_close(semaphore);
        return NULL;
    }
#endif
    return semaphore;
}

bool DestroySharedSimulation(Simulation *simulation) {
    bool success = true;
    for (int i = WORKER_COUNT + 2; i >= 0; --i) {
        sem_t *semaphore = *semaphore_slot(simulation, i);
        if (semaphore == NULL) {
            continue;
        }
#ifdef USE_UNNAMED_SEMAPHORES
        int result = sem_destroy(semaphore);
#else
        int result = sem_close(semaphore);
#endif
        if (result != 0) {
            perror("destroy semaphore");
            success = false;
        }
    }
    if (munmap(simulation, sizeof(*simulation)) != 0) {
        perror("munmap");
        success = false;
    }
    return success;
}

Simulation *CreateSharedSimulation(void) {
    char name[64];
    snprintf(name, sizeof(name), "/water_%ld_memory", (long)getpid());
    int descriptor = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (descriptor == -1) {
        perror("shm_open");
        return NULL;
    }

    // Дочерние процессы наследуют отображение, открывать объект по имени им не нужно
    if (shm_unlink(name) != 0) {
        perror("shm_unlink");
        close(descriptor);
        return NULL;
    }
    if (ftruncate(descriptor, sizeof(Simulation)) != 0) {
        perror("ftruncate");
        close(descriptor);
        return NULL;
    }
    Simulation *simulation = mmap(NULL, sizeof(*simulation), PROT_READ | PROT_WRITE, MAP_SHARED, descriptor, 0);
    int close_result = close(descriptor);
    if (simulation == MAP_FAILED) {
        perror("mmap");
        return NULL;
    }
    if (close_result != 0) {
        perror("close");
        munmap(simulation, sizeof(*simulation));
        return NULL;
    }

    memset(simulation, 0, sizeof(*simulation));
    for (int i = 0; i < WORKER_COUNT + 3; ++i) {
        *semaphore_slot(simulation, i) = create_semaphore(simulation, i);
        if (*semaphore_slot(simulation, i) == NULL) {
            DestroySharedSimulation(simulation);
            return NULL;
        }
    }
    return simulation;
}
