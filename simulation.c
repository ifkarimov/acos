#define _POSIX_C_SOURCE 200809L

#include "simulation.h"
#include "output.h"
#include "ipc.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <time.h>
#include <sys/wait.h>
#include <unistd.h>

static bool wait_semaphore(sem_t *semaphore) {
    int result;
    do {
        result = sem_wait(semaphore);
    } while (result != 0 && errno == EINTR);
    if (result != 0) {
        perror("sem_wait");
        return false;
    }
    return true;
}

// Эти две функции используются только в дочерних процессах
// При ошибке родитель увидит код 2 через waitpid и остановит остальных
static void lock_worker(Simulation *simulation) {
    if (!wait_semaphore(simulation->mutex)) {
        _exit(2);
    }
}

static void unlock_worker(Simulation *simulation) {
    if (sem_post(simulation->mutex) != 0) {
        perror("sem_post");
        _exit(2);
    }
}

// Функции с locked в названии вызываются при захваченном семафоре модели
static void stop_locked(Simulation *simulation) {
    if (simulation->stopping) {
        return;
    }
    simulation->stopping = true;
    for (int i = 0; i < WORKER_COUNT; ++i) {
        if (sem_post(simulation->stop_event[i]) != 0 || sem_post(simulation->start_event) != 0) {
            perror("sem_post");
            simulation->system_error = true;
        }
    }

    // диспетчер может спать на семафоре, его тоже нужно разбудить
    if (sem_post(simulation->dispatcher_event) != 0) {
        perror("sem_post");
        simulation->system_error = true;
    }
}

static void notify_dispatcher_locked(Simulation *simulation) {
    if (sem_post(simulation->dispatcher_event) != 0) {
        perror("sem_post");
        simulation->system_error = true;
        stop_locked(simulation);
    }
}

static bool wait_start_locked(Simulation *simulation) {
    unlock_worker(simulation);
    if (!wait_semaphore(simulation->start_event)) {
        _exit(2);
    }
    lock_worker(simulation);
    return simulation->started && !simulation->stopping;
}

static bool make_deadline(struct timespec *deadline, long delay_ms) {
    if (clock_gettime(CLOCK_REALTIME, deadline) != 0) {
        perror("clock_gettime");
        return false;
    }

    deadline->tv_sec += delay_ms / 1000;
    deadline->tv_nsec += (delay_ms % 1000) * 1000000L;
    if (deadline->tv_nsec >= 1000000000L) {
        ++deadline->tv_sec;
        deadline->tv_nsec -= 1000000000L;
    }
    return true;
}

// Во время ожидания семафор модели освобождается, остальные процессы продолжают работу
static bool wait_step_locked(ProcessData *data, long delay_ms) {
    Simulation *simulation = data->simulation;
    if (simulation->stopping) {
        return false;
    }
    struct timespec deadline;
    if (!make_deadline(&deadline, delay_ms)) {
        simulation->system_error = true;
        stop_locked(simulation);
        return false;
    }

    unlock_worker(simulation);
    int result;
    do {
        result = sem_timedwait(simulation->stop_event[data->index], &deadline);
    } while (result != 0 && errno == EINTR);
    int error = result == 0 ? 0 : errno;
    lock_worker(simulation);
    if (error != 0 && error != ETIMEDOUT) {
        errno = error;
        perror("sem_timedwait");
        simulation->system_error = true;
        stop_locked(simulation);
    }
    return !simulation->stopping;
}

static void ReservoirProcess(ProcessData *data) {
    Simulation *simulation = data->simulation;

    lock_worker(simulation);
    if (wait_start_locked(simulation)) {
        do {
            ReservoirStep(&simulation->system, data->id, simulation->tick);
            if (simulation->system.fatal_accident) {
                stop_locked(simulation);
            }
        } while (wait_step_locked(data, simulation->delay_ms));
    }
    unlock_worker(simulation);
}

static void PumpProcess(ProcessData *data) {
    Simulation *simulation = data->simulation;

    lock_worker(simulation);
    if (wait_start_locked(simulation)) {
        do {
            PumpStep(&simulation->system, data->id, simulation->tick);
            if (simulation->system.fatal_accident) {
                stop_locked(simulation);
            }
        } while (wait_step_locked(data, simulation->delay_ms));
    }
    unlock_worker(simulation);
}

static void DistrictProcess(ProcessData *data) {
    Simulation *simulation = data->simulation;

    lock_worker(simulation);
    if (wait_start_locked(simulation)) {
        // начальную потребность задали до запуска процессов
        while (wait_step_locked(data, simulation->delay_ms)) {
            DistrictFinish(&simulation->system, data->id, simulation->tick);
            DistrictPrepare(&simulation->system, data->id, simulation->tick);
            notify_dispatcher_locked(simulation);
        }
    }
    unlock_worker(simulation);
}

static void SensorProcess(ProcessData *data) {
    Simulation *simulation = data->simulation;

    lock_worker(simulation);
    if (wait_start_locked(simulation)) {
        do {
            GenerateMessage(&simulation->system, data->id, simulation->tick, &data->seed);
            if (simulation->system.fatal_accident) {
                stop_locked(simulation);
            } else {
                notify_dispatcher_locked(simulation);
            }
        } while (wait_step_locked(data, simulation->delay_ms * MESSAGE_PERIOD));
    }
    unlock_worker(simulation);
}

static void DispatcherProcess(ProcessData *data) {
    Simulation *simulation = data->simulation;

    lock_worker(simulation);
    bool running = wait_start_locked(simulation);
    unlock_worker(simulation);

    while (running) {
        // семафор модели не захвачен, пока диспетчер ожидает уведомление
        int result;
        do {
            result = sem_wait(simulation->dispatcher_event);
        } while (result != 0 && errno == EINTR);

        lock_worker(simulation);
        if (result != 0) {
            perror("sem_wait");
            simulation->system_error = true;
            stop_locked(simulation);
        }

        running = !simulation->stopping;
        if (running) {
            DispatcherReceiveMessages(&simulation->system, &simulation->dispatcher, simulation->tick);
            DispatcherControl(&simulation->system, &simulation->dispatcher, simulation->tick);
        }
        unlock_worker(simulation);
    }
}

// Проверяем только своих детей, чтобы не забрать статус постороннего процесса
static bool check_children(pid_t *children, int created) {
    bool success = true;
    for (int i = 0; i < created; ++i) {
        if (children[i] == 0) {
            continue;
        }
        int status;
        pid_t result;
        do {
            result = waitpid(children[i], &status, WNOHANG);
        } while (result == -1 && errno == EINTR);
        if (result == -1) {
            perror("waitpid");
            success = false;
        } else if (result != 0) {
            if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
                fprintf(stderr, "Worker process %ld stopped unexpectedly\n", (long)children[i]);
                success = false;
            }
            children[i] = 0;
        }
    }
    return success;
}

// Семафор не восстанавливается автоматически после гибели владельца
// Поэтому родитель проверяет детей и во время ожидания блокировки
static bool lock_parent(Simulation *simulation, pid_t *children, int created) {
    while (check_children(children, created)) {
        struct timespec deadline;
        if (!make_deadline(&deadline, 50)) {
            return false;
        }
        if (sem_timedwait(simulation->mutex, &deadline) == 0) {
            return true;
        }
        if (errno != EINTR && errno != ETIMEDOUT) {
            perror("sem_timedwait");
            return false;
        }
    }
    return false;
}

static bool unlock_parent(Simulation *simulation) {
    if (sem_post(simulation->mutex) != 0) {
        perror("sem_post");
        return false;
    }
    return true;
}

static bool finish_children(pid_t *children, int created, bool force) {
    bool success = true;
    while (!force) {
        if (!check_children(children, created)) {
            success = false;
            force = true;
            break;
        }
        bool remaining = false;
        for (int i = 0; i < created; ++i) {
            if (children[i] != 0) {
                remaining = true;
            }
        }
        if (!remaining) {
            return success;
        }
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 10000000L};
        if (nanosleep(&pause, NULL) != 0 && errno != EINTR) {
            perror("nanosleep");
            success = false;
            force = true;
        }
    }

    // Состояние могло остаться заблокированным погибшим процессом
    for (int i = 0; i < created; ++i) {
        if (children[i] != 0 && kill(children[i], SIGKILL) != 0 && errno != ESRCH) {
            perror("kill");
            success = false;
        }
    }
    for (int i = 0; i < created; ++i) {
        if (children[i] == 0) {
            continue;
        }
        pid_t result;
        do {
            result = waitpid(children[i], NULL, 0);
        } while (result == -1 && errno == EINTR);
        if (result == -1) {
            perror("waitpid");
            success = false;
        }
    }
    return success;
}

int RunSimulation(int max_ticks, long delay_ms, unsigned int seed, const volatile sig_atomic_t *stop_requested) {
    Simulation *simulation = CreateSharedSimulation();
    if (simulation == NULL) {
        return 2;
    }
    ModelInit(&simulation->system);
    DispatcherInit(&simulation->dispatcher, &simulation->system);
    // нулевая задержка включает быстрый режим, но не бесконечный активный цикл
    simulation->delay_ms = delay_ms == 0 ? 1 : delay_ms;

    // Сигналы обрабатывает родитель, дети наследуют заблокированную маску
    sigset_t signals;
    sigset_t old_mask;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    if (sigprocmask(SIG_BLOCK, &signals, &old_mask) != 0) {
        perror("sigprocmask");
        DestroySharedSimulation(simulation);
        return 2;
    }

    for (int d = 0; d < DISTRICT_COUNT; ++d) {
        DistrictPrepare(&simulation->system, d, 0);
    }

    pid_t children[WORKER_COUNT] = {0};
    int created = 0;
    bool success = fflush(NULL) == 0;
    void (*functions[])(ProcessData *) = {ReservoirProcess, DistrictProcess, PumpProcess, SensorProcess, DispatcherProcess};
    int counts[] = {RESERVOIR_COUNT, DISTRICT_COUNT, PUMP_COUNT, RESERVOIR_COUNT, 1};
    const char *roles[] = {"reservoir", "district", "pump", "sensor", "dispatcher"};

    for (int type = 0; type < 5 && success; ++type) {
        for (int id = 0; id < counts[type]; ++id) {
            ProcessData data = {
                .simulation = simulation,
                .id = id,
                .seed = seed + (unsigned int)id * 2654435761u,
                .index = created,
            };
            pid_t child = fork();
            if (child == -1) {
                perror("fork");
                success = false;
                break;
            }
            if (child == 0) {
                functions[type](&data);
                // Не запускаем обработчики exit и не повторяем буферы родителя
                int result = fflush(stdout) == EOF || ferror(stdout) ? 2 : 0;
                _exit(result);
            }
            children[created] = child;
            printf("[PROCESS] %s %d: pid = %ld\n", roles[type], id, (long)child);
            ++created;
        }
    }

    if (sigprocmask(SIG_SETMASK, &old_mask, NULL) != 0) {
        perror("sigprocmask");
        success = false;
    }

    bool locked = lock_parent(simulation, children, created);
    if (!locked) {
        success = false;
    }
    if (!success) {
        if (locked) {
            simulation->system_error = true;
            stop_locked(simulation);
        }
    } else if (*stop_requested) {
        stop_locked(simulation);
    } else {
        printf("[PROCESSES] Started %d worker processes\n", created);
        simulation->started = true;
        for (int i = 0; i < created; ++i) {
            if (sem_post(simulation->start_event) != 0) {
                perror("sem_post");
                simulation->system_error = true;
                stop_locked(simulation);
                break;
            }
        }
        notify_dispatcher_locked(simulation);
    }
    if (locked && !unlock_parent(simulation)) {
        success = false;
    }

    long remaining_ms = simulation->delay_ms;
    while (success) {
        if (!lock_parent(simulation, children, created)) {
            success = false;
            break;
        }
        if (*stop_requested) {
            stop_locked(simulation);
        }
        if (!simulation->stopping && remaining_ms == 0) {
            PrintStatus(&simulation->system, &simulation->dispatcher, simulation->tick);
            ++simulation->tick;
            if (max_ticks != 0 && simulation->tick >= max_ticks) {
                stop_locked(simulation);
            } else if (simulation->tick >= INT_MAX - 2 * MAX_MESSAGE_DELAY) {
                printf("[ERROR] Simulation tick limit is reached\n");
                simulation->system_error = true;
                stop_locked(simulation);
            } else {
                // сообщения с задержкой могли стать доступными на новом такте
                notify_dispatcher_locked(simulation);
            }
            remaining_ms = simulation->delay_ms;
        }
        bool stopping = simulation->stopping;
        success = !simulation->system_error;
        if (!unlock_parent(simulation)) {
            success = false;
        }
        if (stopping || !success) {
            break;
        }

        // Небольшие интервалы позволяют быстро заметить сигнал или гибель ребёнка
        long part = remaining_ms < 50 ? remaining_ms : 50;
        struct timespec pause = {.tv_sec = part / 1000, .tv_nsec = (part % 1000) * 1000000L};
        while (nanosleep(&pause, &pause) != 0) {
            if (errno != EINTR) {
                perror("nanosleep");
                success = false;
                break;
            }
            if (*stop_requested) {
                break;
            }
        }
        remaining_ms -= part;
    }

    if (!finish_children(children, created, !success)) {
        success = false;
    }

    // Все дети завершены. При системной ошибке снимок может быть неполным
    if (!success) {
        printf("[FINISH] Simulation stopped because of a system error\n");
        DestroySharedSimulation(simulation);
        return 2;
    }
    if (simulation->system.fatal_accident) {
        printf("[FINISH] Simulation stopped because of a fatal accident!\n");
    } else if (*stop_requested) {
        printf("[FINISH] Interrupted by user (signal %d)\n", (int)*stop_requested);
    } else {
        printf("[FINISH] Requested tick count is completed\n");
    }

    PrintStatus(&simulation->system, &simulation->dispatcher, simulation->tick);
    PrintSummary(&simulation->system);
    int result = simulation->system.fatal_accident ? 1 : 0;

    if (!DestroySharedSimulation(simulation)) {
        return 2;
    }
    return result;
}
