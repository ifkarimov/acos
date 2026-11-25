#define _POSIX_C_SOURCE 200809L

#include "model.h"
#include "dispatcher.h"
#include "output.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define PARTICIPANTS 12
#define EVENTS (PARTICIPANTS + 1)

// В общей памяти нет адресов семафоров. Поля name всегда равны NULL
typedef struct {
    WaterSystem system;
    Dispatcher dispatcher;
    pid_t owner;
    long long heartbeat[PARTICIPANTS];
    long long owner_heartbeat;
    bool registered[PARTICIPANTS];
    bool active[PARTICIPANTS];
    bool started;
    bool stopping;
    bool error;
    int tick;
    long delay_ms;
    unsigned int seed;
} SharedState;

static volatile sig_atomic_t stop_requested = 0;
static const char *roles[] = {"reservoir", "district", "pump", "sensor", "dispatcher"};
static const int counts[] = {RESERVOIR_COUNT, DISTRICT_COUNT, PUMP_COUNT, RESERVOIR_COUNT, 1};

static void on_signal(int signal_number) {
    stop_requested = signal_number;
}

static long number(const char *text, long maximum) {
    char *end;
    errno = 0;
    long value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value < 0 || value > maximum) {
        fprintf(stderr, "Invalid number: %s\n", text);
        exit(2);
    }
    return value;
}

static long long milliseconds(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        perror("clock_gettime");
        exit(2);
    }
    return (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

// Возвращает 1 при уведомлении, 0 при тайм-ауте, -1 при ошибке
static int wait_event(sem_t *event, long delay_ms) {
    struct timespec deadline;
    if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) {
        return -1;
    }
    deadline.tv_sec += delay_ms / 1000;
    deadline.tv_nsec += delay_ms % 1000 * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        ++deadline.tv_sec;
        deadline.tv_nsec -= 1000000000L;
    }
    int result;
    do {
        result = sem_timedwait(event, &deadline);
    } while (result != 0 && errno == EINTR && !stop_requested);
    if (result == 0) {
        return 1;
    }
    return errno == ETIMEDOUT || errno == EINTR ? 0 : -1;
}

static bool lock_state(sem_t *mutex) {
    // Ограниченное ожидание позволяет выйти, если владелец семафора погиб
    for (int attempt = 0; attempt < 20; ++attempt) {
        int result = wait_event(mutex, 50);
        if (result == 1) {
            return true;
        }
        if (result == -1) {
            break;
        }
    }
    fprintf(stderr, "Cannot acquire shared state semaphore\n");
    return false;
}

static bool post_event(sem_t *event) {
    if (sem_post(event) != 0) {
        perror("sem_post");
        return false;
    }
    return true;
}

static void stop_locked(SharedState *shared, sem_t **events) {
    shared->stopping = true;
    for (int i = 0; i < EVENTS; ++i) {
        if (!post_event(events[i])) {
            shared->error = true;
        }
    }
}

// Названия берём из локальной конфигурации, адреса другой программы не используем
static void load_system(WaterSystem *system, const SharedState *shared, const WaterSystem *configuration) {
    *system = shared->system;
    for (int i = 0; i < RESERVOIR_COUNT; ++i) {
        system->reservoirs[i].name = configuration->reservoirs[i].name;
    }
    for (int i = 0; i < DISTRICT_COUNT; ++i) {
        system->districts[i].name = configuration->districts[i].name;
    }
    for (int i = 0; i < PUMP_COUNT; ++i) {
        system->pumps[i].name = configuration->pumps[i].name;
    }
    for (int i = 0; i < PIPE_COUNT; ++i) {
        system->pipes[i].name = configuration->pipes[i].name;
    }
}

static void save_system(SharedState *shared, const WaterSystem *system) {
    shared->system = *system;
    for (int i = 0; i < RESERVOIR_COUNT; ++i) {
        shared->system.reservoirs[i].name = NULL;
    }
    for (int i = 0; i < DISTRICT_COUNT; ++i) {
        shared->system.districts[i].name = NULL;
    }
    for (int i = 0; i < PUMP_COUNT; ++i) {
        shared->system.pumps[i].name = NULL;
    }
    for (int i = 0; i < PIPE_COUNT; ++i) {
        shared->system.pipes[i].name = NULL;
    }
}

static int run(SharedState *shared, sem_t *mutex, sem_t **events, int type, int id, int index, int max_ticks) {
    bool controller = type == -1;
    WaterSystem configuration;
    WaterSystem system;
    ModelInit(&configuration);
    if (!lock_state(mutex)) {
        return 2;
    }
    if (!controller) {
        if (shared->stopping || shared->started || shared->registered[index]) {
            fprintf(stderr, "Participant is already registered or session is closed\n");
            post_event(mutex);
            return 2;
        }
        shared->registered[index] = true;
        shared->active[index] = true;
        shared->heartbeat[index] = milliseconds();
        printf("[REGISTER] %s %d: pid = %ld\n", roles[type], id, (long)getpid());
    }
    unsigned int seed = shared->seed + (unsigned int)id * 2654435761u;
    long period = shared->delay_ms * (type == 3 ? MESSAGE_PERIOD : 1);
    pid_t owner = shared->owner;
    if (!post_event(mutex)) {
        return 2;
    }

    long long next = 0;
    long long startup_deadline = milliseconds() + 60000;
    long long shutdown_deadline = 0;
    bool notified = true;
    int result = 0;
    while (true) {
        if (!lock_state(mutex)) {
            result = 2;
            break;
        }
        long long now = milliseconds();
        if (controller) {
            shared->owner_heartbeat = now;
            for (int i = 0; i < PARTICIPANTS; ++i) {
                if (shared->active[i] && now - shared->heartbeat[i] > 3000 && !shared->stopping) {
                    fprintf(stderr, "Participant %d stopped responding\n", i);
                    shared->error = true;
                    stop_locked(shared, events);
                }
            }
        } else {
            shared->heartbeat[index] = now;
            if (now - shared->owner_heartbeat > 3000 && !shared->stopping) {
                shared->error = true;
                stop_locked(shared, events);
            }
        }
        load_system(&system, shared, &configuration);
        if (stop_requested && !shared->stopping) {
            printf("[SIGNAL] pid = %ld, signal = %d\n", (long)getpid(), (int)stop_requested);
            stop_locked(shared, events);
        }
        if (!controller && kill(owner, 0) == -1 && errno == ESRCH) {
            shared->error = true;
            stop_locked(shared, events);
        }
        if (controller && !shared->started && !shared->stopping) {
            int connected = 0;
            for (int i = 0; i < PARTICIPANTS; ++i) {
                connected += shared->registered[i];
            }
            if (connected == PARTICIPANTS) {
                shared->started = true;
                printf("[INDEPENDENT] Started 12 participants\n");
                for (int i = 0; i < EVENTS; ++i) {
                    if (!post_event(events[i])) {
                        shared->error = true;
                        stop_locked(shared, events);
                    }
                }
            } else if (now >= startup_deadline) {
                fprintf(stderr, "Not all participants connected within 60 seconds\n");
                shared->error = true;
                stop_locked(shared, events);
            }
        }
        if (shared->started && !shared->stopping) {
            if (next == 0) {
                next = now + (controller || type == 1 ? period : 0);
            }
            if ((type == 4 && notified) || (type != 4 && now >= next)) {
                if (controller) {
                    PrintStatus(&system, &shared->dispatcher, shared->tick);
                    ++shared->tick;
                    if ((max_ticks != 0 && shared->tick >= max_ticks) || shared->tick >= INT_MAX - 2 * MAX_MESSAGE_DELAY) {
                        stop_locked(shared, events);
                    }
                } else if (type == 0) {
                    ReservoirStep(&system, id, shared->tick);
                } else if (type == 1) {
                    DistrictFinish(&system, id, shared->tick);
                    DistrictPrepare(&system, id, shared->tick);
                } else if (type == 2) {
                    PumpStep(&system, id, shared->tick);
                } else if (type == 3) {
                    GenerateMessage(&system, id, shared->tick, &seed);
                } else {
                    DispatcherReceiveMessages(&system, &shared->dispatcher, shared->tick);
                    DispatcherControl(&system, &shared->dispatcher, shared->tick);
                }
                if (controller || type == 1 || type == 3) {
                    if (!post_event(events[PARTICIPANTS - 1])) {
                        shared->error = true;
                        stop_locked(shared, events);
                    }
                }
                if (system.fatal_accident) {
                    stop_locked(shared, events);
                }
                save_system(shared, &system);
                next = now + period;
            }
        }
        if (ferror(stdout)) {
            shared->error = true;
            stop_locked(shared, events);
        }
        bool finished = false;
        if (shared->stopping) {
            if (!controller) {
                shared->active[index] = false;
                finished = true;
            } else {
                if (shutdown_deadline == 0) {
                    shutdown_deadline = now + 3000;
                }
                int active = 0;
                for (int i = 0; i < PARTICIPANTS; ++i) {
                    active += shared->active[i];
                }
                finished = active == 0 || now >= shutdown_deadline;
                if (active != 0 && finished) {
                    shared->error = true;
                }
                if (finished) {
                    printf("[FINISH] %s\n", shared->error ? "System error" : system.fatal_accident ? "Fatal accident" : "Independent simulation completed");
                    PrintStatus(&system, &shared->dispatcher, shared->tick);
                    PrintSummary(&system);
                }
            }
            result = shared->error ? 2 : system.fatal_accident ? 1 : 0;
        }
        bool stopping = shared->stopping;
        bool started = shared->started;
        if (!post_event(mutex)) {
            return 2;
        }
        if (finished) {
            break;
        }
        long delay = 50;
        if (started && !stopping && type != 4 && next - now < delay) {
            delay = next > now ? (long)(next - now) : 0;
        }
        int event = wait_event(events[controller ? PARTICIPANTS : index], delay);
        if (event == -1) {
            result = 2;
            break;
        }
        notified = event == 1;
    }
    return result;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s session controller [ticks delay_ms seed] | session role id\n", argv[0]);
        return 2;
    }
    size_t length = strlen(argv[1]);
    if (length == 0 || length > 32 || strspn(argv[1], "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") != length) {
        fprintf(stderr, "Session must contain 1-32 letters, digits or underscores\n");
        return 2;
    }
    bool controller = strcmp(argv[2], "controller") == 0;
    int type = -1;
    int index = 0;
    int id = 0;
    int max_ticks = 30;
    long delay_ms = 1000;
    unsigned int seed = (unsigned int)time(NULL);
    if (controller) {
        if (argc > 6) {
            return 2;
        }
        if (argc > 3) {
            max_ticks = (int)number(argv[3], INT_MAX - 2 * MAX_MESSAGE_DELAY);
        }
        if (argc > 4) {
            delay_ms = number(argv[4], 3600000);
        }
        if (argc > 5) {
            seed = (unsigned int)number(argv[5], LONG_MAX < UINT_MAX ? LONG_MAX : (long)UINT_MAX);
        }
    } else {
        for (int i = 0; i < 5; ++i) {
            if (strcmp(argv[2], roles[i]) == 0) {
                type = i;
                break;
            }
            index += counts[i];
        }
        if (type == -1 || argc != 4) {
            fprintf(stderr, "Unknown role or invalid argument count\n");
            return 2;
        }
        id = (int)number(argv[3], counts[type] - 1);
        index += id;
    }
    struct sigaction action = {0};
    action.sa_handler = on_signal;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) != 0 || sigaction(SIGTERM, &action, NULL) != 0) {
        return 2;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    char memory_name[64];
    char names[EVENTS + 1][64];
    snprintf(memory_name, sizeof(memory_name), "/water_ind_%s_memory", argv[1]);
    for (int i = 0; i < EVENTS + 1; ++i) {
        snprintf(names[i], sizeof(names[i]), "/water_ind_%s_%d", argv[1], i);
    }
    int descriptor = shm_open(memory_name, controller ? O_CREAT | O_EXCL | O_RDWR : O_RDWR, 0600);
    if (descriptor == -1) {
        perror("shm_open");
        return 2;
    }
    int result = 2;
    SharedState *shared = MAP_FAILED;
    sem_t *semaphores[EVENTS + 1] = {0};
    int opened = 0;
    if (controller && ftruncate(descriptor, sizeof(SharedState)) != 0) {
        goto cleanup;
    }
    struct stat info;
    if (fstat(descriptor, &info) != 0 || info.st_size != sizeof(SharedState)) {
        fprintf(stderr, "Session is not ready\n");
        goto cleanup;
    }
    shared = mmap(NULL, sizeof(*shared), PROT_READ | PROT_WRITE, MAP_SHARED, descriptor, 0);
    if (shared == MAP_FAILED) {
        goto cleanup;
    }
    for (int i = 0; i < EVENTS + 1; ++i) {
        semaphores[i] = sem_open(names[i], controller ? O_CREAT | O_EXCL : 0, 0600, 0);
        if (semaphores[i] == SEM_FAILED) {
            perror("sem_open");
            goto cleanup;
        }
        ++opened;
    }
    if (controller) {
        memset(shared, 0, sizeof(*shared));
        WaterSystem system;
        ModelInit(&system);
        for (int d = 0; d < DISTRICT_COUNT; ++d) {
            DistrictPrepare(&system, d, 0);
        }
        DispatcherInit(&shared->dispatcher, &system);
        save_system(shared, &system);
        shared->owner = getpid();
        shared->owner_heartbeat = milliseconds();
        shared->delay_ms = delay_ms == 0 ? 1 : delay_ms;
        shared->seed = seed;
        if (!post_event(semaphores[0])) {
            goto cleanup;
        }
        printf("[READY] Session %s, controller pid = %ld\n", argv[1], (long)getpid());
    }
    result = run(shared, semaphores[0], &semaphores[1], type, id, index, max_ticks);
cleanup:
    if (controller) {
        for (int i = 0; i < opened; ++i) {
            if (sem_unlink(names[i]) != 0) {
                result = 2;
            }
        }
        if (shm_unlink(memory_name) != 0) {
            result = 2;
        }
    }
    for (int i = 0; i < opened; ++i) {
        if (sem_close(semaphores[i]) != 0) {
            result = 2;
        }
    }
    if (shared != MAP_FAILED && munmap(shared, sizeof(*shared)) != 0) {
        result = 2;
    }
    if (close(descriptor) != 0) {
        result = 2;
    }
    return result;
}
