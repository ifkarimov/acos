#define _POSIX_C_SOURCE 200809L

#include "simulation.h"
#include "output.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

// Функции с locked в названии вызываются при захваченном мьютексе
static void stop_locked(Simulation *simulation) {
    if (simulation->stopping) {
        return;
    }
    simulation->stopping = true;
    pthread_cond_broadcast(&simulation->condition);

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
    while (!simulation->started && !simulation->stopping) {
        int error = pthread_cond_wait(&simulation->condition, &simulation->mutex);
        if (error != 0) {
            fprintf(stderr, "pthread_cond_wait: %s\n", strerror(error));
            simulation->system_error = true;
            stop_locked(simulation);
        }
    }
    return !simulation->stopping;
}

// Во время ожидания мьютекс освобождается, остальные потоки продолжают работу
static bool wait_step_locked(Simulation *simulation, long delay_ms) {
    struct timespec deadline;
    if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) {
        perror("clock_gettime");
        simulation->system_error = true;
        stop_locked(simulation);
        return false;
    }

    deadline.tv_sec += delay_ms / 1000;
    deadline.tv_nsec += (delay_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        ++deadline.tv_sec;
        deadline.tv_nsec -= 1000000000L;
    }

    while (!simulation->stopping) {
        int error = pthread_cond_timedwait(&simulation->condition, &simulation->mutex, &deadline);
        if (error == ETIMEDOUT) {
            return !simulation->stopping;
        }
        if (error != 0) {
            fprintf(stderr, "pthread_cond_timedwait: %s\n", strerror(error));
            simulation->system_error = true;
            stop_locked(simulation);
        }
        // после случайного пробуждения ждём до того же момента времени
    }
    return false;
}

static void *ReservoirThread(void *argument) {
    ThreadData *data = argument;
    Simulation *simulation = data->simulation;

    pthread_mutex_lock(&simulation->mutex);
    if (wait_start_locked(simulation)) {
        do {
            ReservoirStep(&simulation->system, data->id, simulation->tick);
            if (simulation->system.fatal_accident) {
                stop_locked(simulation);
            }
        } while (wait_step_locked(simulation, simulation->delay_ms));
    }
    pthread_mutex_unlock(&simulation->mutex);
    return NULL;
}

static void *PumpThread(void *argument) {
    ThreadData *data = argument;
    Simulation *simulation = data->simulation;

    pthread_mutex_lock(&simulation->mutex);
    if (wait_start_locked(simulation)) {
        do {
            PumpStep(&simulation->system, data->id, simulation->tick);
            if (simulation->system.fatal_accident) {
                stop_locked(simulation);
            }
        } while (wait_step_locked(simulation, simulation->delay_ms));
    }
    pthread_mutex_unlock(&simulation->mutex);
    return NULL;
}

static void *DistrictThread(void *argument) {
    ThreadData *data = argument;
    Simulation *simulation = data->simulation;

    pthread_mutex_lock(&simulation->mutex);
    if (wait_start_locked(simulation)) {
        // начальную потребность задали до запуска потоков
        while (wait_step_locked(simulation, simulation->delay_ms)) {
            DistrictFinish(&simulation->system, data->id, simulation->tick);
            DistrictPrepare(&simulation->system, data->id, simulation->tick);
            notify_dispatcher_locked(simulation);
        }
    }
    pthread_mutex_unlock(&simulation->mutex);
    return NULL;
}

static void *SensorThread(void *argument) {
    ThreadData *data = argument;
    Simulation *simulation = data->simulation;

    pthread_mutex_lock(&simulation->mutex);
    if (wait_start_locked(simulation)) {
        do {
            GenerateMessage(&simulation->system, data->id, simulation->tick, &data->seed);
            if (simulation->system.fatal_accident) {
                stop_locked(simulation);
            } else {
                notify_dispatcher_locked(simulation);
            }
        } while (wait_step_locked(simulation, simulation->delay_ms * MESSAGE_PERIOD));
    }
    pthread_mutex_unlock(&simulation->mutex);
    return NULL;
}

static void *DispatcherThread(void *argument) {
    ThreadData *data = argument;
    Simulation *simulation = data->simulation;

    pthread_mutex_lock(&simulation->mutex);
    bool running = wait_start_locked(simulation);
    pthread_mutex_unlock(&simulation->mutex);

    while (running) {
        // мьютекс не захвачен, пока диспетчер ожидает уведомление
        int result;
        do {
            result = sem_wait(simulation->dispatcher_event);
        } while (result != 0 && errno == EINTR);

        pthread_mutex_lock(&simulation->mutex);
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
        pthread_mutex_unlock(&simulation->mutex);
    }
    return NULL;
}

static bool create_thread(pthread_t *thread, ThreadData *data, void *(*function)(void *)) {
    int error = pthread_create(thread, NULL, function, data);
    if (error != 0) {
        fprintf(stderr, "pthread_create: %s\n", strerror(error));
        return false;
    }
    return true;
}

static sem_t *create_semaphore(void) {
    char name[64];
    snprintf(name, sizeof(name), "/water_%ld", (long)getpid());
    sem_t *semaphore = sem_open(name, O_CREAT | O_EXCL, 0600, 0);
    if (semaphore == SEM_FAILED) {
        perror("sem_open");
        return SEM_FAILED;
    }

    // убираем имя сразу, сам семафор остаётся доступен до sem_close
    if (sem_unlink(name) != 0) {
        perror("sem_unlink");
        sem_close(semaphore);
        return SEM_FAILED;
    }
    return semaphore;
}

static bool init_simulation(Simulation *simulation, long delay_ms) {
    memset(simulation, 0, sizeof(*simulation));
    ModelInit(&simulation->system);
    DispatcherInit(&simulation->dispatcher, &simulation->system);
    // нулевая задержка включает быстрый режим, но не бесконечный активный цикл
    simulation->delay_ms = delay_ms == 0 ? 1 : delay_ms;

    int error = pthread_mutex_init(&simulation->mutex, NULL);
    if (error != 0) {
        fprintf(stderr, "pthread_mutex_init: %s\n", strerror(error));
        return false;
    }
    error = pthread_cond_init(&simulation->condition, NULL);
    if (error != 0) {
        fprintf(stderr, "pthread_cond_init: %s\n", strerror(error));
        pthread_mutex_destroy(&simulation->mutex);
        return false;
    }
    simulation->dispatcher_event = create_semaphore();
    if (simulation->dispatcher_event == SEM_FAILED) {
        pthread_cond_destroy(&simulation->condition);
        pthread_mutex_destroy(&simulation->mutex);
        return false;
    }
    return true;
}

int RunSimulation(int max_ticks, long delay_ms, unsigned int seed, const volatile sig_atomic_t *stop_requested) {
    Simulation simulation;
    if (!init_simulation(&simulation, delay_ms)) {
        return 2;
    }

    // сигналы получает только главный поток, рабочие наследуют заблокированную маску
    sigset_t signals;
    sigset_t old_mask;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    int error = pthread_sigmask(SIG_BLOCK, &signals, &old_mask);
    if (error != 0) {
        fprintf(stderr, "pthread_sigmask: %s\n", strerror(error));
        sem_close(simulation.dispatcher_event);
        pthread_cond_destroy(&simulation.condition);
        pthread_mutex_destroy(&simulation.mutex);
        return 2;
    }

    for (int d = 0; d < DISTRICT_COUNT; ++d) {
        DistrictPrepare(&simulation.system, d, 0);
    }

    pthread_t threads[WORKER_COUNT];
    ThreadData data[WORKER_COUNT];
    int created = 0;
    bool success = true;
    void *(*functions[])(void *) = {ReservoirThread, DistrictThread, PumpThread, SensorThread, DispatcherThread};
    int counts[] = {RESERVOIR_COUNT, DISTRICT_COUNT, PUMP_COUNT, RESERVOIR_COUNT, 1};

    for (int type = 0; type < 5 && success; ++type) {
        for (int id = 0; id < counts[type]; ++id) {
            data[created] = (ThreadData){
                .simulation = &simulation,
                .id = id,
                .seed = seed + (unsigned int)id * 2654435761u,
            };
            if (!create_thread(&threads[created], &data[created], functions[type])) {
                success = false;
                break;
            }
            ++created;
        }
    }

    error = pthread_sigmask(SIG_SETMASK, &old_mask, NULL);
    if (error != 0) {
        fprintf(stderr, "pthread_sigmask: %s\n", strerror(error));
        success = false;
    }

    pthread_mutex_lock(&simulation.mutex);
    if (!success) {
        simulation.system_error = true;
        stop_locked(&simulation);
    } else if (*stop_requested) {
        stop_locked(&simulation);
    } else {
        printf("[THREADS] Started %d worker threads\n", created);
        simulation.started = true;
        pthread_cond_broadcast(&simulation.condition);
        notify_dispatcher_locked(&simulation);
    }

    while (!simulation.stopping) {
        long remaining_ms = simulation.delay_ms;
        // небольшие интервалы позволяют быстро заметить Ctrl + C
        while (remaining_ms > 0 && !simulation.stopping && !*stop_requested) {
            long part = remaining_ms < 50 ? remaining_ms : 50;
            if (!wait_step_locked(&simulation, part)) {
                break;
            }
            remaining_ms -= part;
        }

        if (*stop_requested) {
            stop_locked(&simulation);
        }
        if (simulation.stopping) {
            break;
        }

        PrintStatus(&simulation.system, &simulation.dispatcher, simulation.tick);
        ++simulation.tick;
        if (max_ticks != 0 && simulation.tick >= max_ticks) {
            stop_locked(&simulation);
        } else if (simulation.tick >= INT_MAX - 2 * MAX_MESSAGE_DELAY) {
            printf("[ERROR] Simulation tick limit is reached\n");
            simulation.system_error = true;
            stop_locked(&simulation);
        } else {
            // сообщения с задержкой могли стать доступными на новом такте
            notify_dispatcher_locked(&simulation);
        }
    }
    pthread_mutex_unlock(&simulation.mutex);

    for (int i = 0; i < created; ++i) {
        error = pthread_join(threads[i], NULL);
        if (error != 0) {
            // нельзя разрушать общие данные, если поток ещё может их использовать
            fprintf(stderr, "pthread_join: %s\n", strerror(error));
            fflush(NULL);
            _Exit(2);
        }
    }

    // все рабочие потоки завершены, мьютекс больше не нужен для чтения данных
    if (simulation.system_error) {
        printf("[FINISH] Simulation stopped because of a system error\n");
    } else if (simulation.system.fatal_accident) {
        printf("[FINISH] Simulation stopped because of a fatal accident!\n");
    } else if (*stop_requested) {
        printf("[FINISH] Interrupted by user (signal %d)\n", (int)*stop_requested);
    } else {
        printf("[FINISH] Requested tick count is completed\n");
    }

    PrintStatus(&simulation.system, &simulation.dispatcher, simulation.tick);
    PrintSummary(&simulation.system);

    if (sem_close(simulation.dispatcher_event) != 0) {
        perror("sem_close");
        simulation.system_error = true;
    }
    pthread_cond_destroy(&simulation.condition);
    pthread_mutex_destroy(&simulation.mutex);

    if (simulation.system_error) {
        return 2;
    }
    return simulation.system.fatal_accident ? 1 : 0;
}
