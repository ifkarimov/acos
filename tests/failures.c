#define _POSIX_C_SOURCE 200809L

#include "model.h"

#include <errno.h>
#include <semaphore.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>

pid_t __real_fork(void);
int __real_sem_wait(sem_t *semaphore);
void __real_ModelInit(WaterSystem *system);

static bool child_process = false;

// Обёртки подключаются только к тестовой сборке через --wrap
pid_t __wrap_fork(void) {
    static int calls = 0;
    const char *failure = getenv("TEST_FAIL_FORK");
    ++calls;
    if (failure != NULL && calls == atoi(failure)) {
        errno = EAGAIN;
        return -1;
    }
    pid_t result = __real_fork();
    if (result == 0) {
        child_process = true;
    }
    return result;
}

int __wrap_sem_wait(sem_t *semaphore) {
    int result = __real_sem_wait(semaphore);
    // Первый sem_wait ребёнка захватывает семафор модели
    if (result == 0 && child_process && getenv("TEST_DIE_WITH_LOCK") != NULL) {
        _exit(2);
    }
    return result;
}

void __wrap_ModelInit(WaterSystem *system) {
    __real_ModelInit(system);
    if (getenv("TEST_ACCIDENT") != NULL) {
        system->reservoirs[0].level = system->reservoirs[0].capacity;
        system->power_limit = 0.0;
    }
}
