#define _POSIX_C_SOURCE 200809L

#include "dispatcher.h"
#include "model.h"
#include "output.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static volatile sig_atomic_t stop_requested = 0;

static void on_sigint(int signal_number) {
    (void)signal_number; // избегаем предупреждение компилятора, т.к параметр фукнции обязателен, но мы его не используем
    stop_requested = 1;
}

static long parse_nonnegative_long(const char *text, const char *name) {
    char *end = NULL;
    errno = 0;

    long value = strtol(text, &end, 10);

    if (errno != 0 || end == text || *end != '\0' || value < 0) {
        fprintf(stderr, "Invalid %s: %s\n", name, text);
        exit(2);
    }

    return value;
}

static void sleep_ms(long delay_ms) {
    if (delay_ms <= 0) {
        return;
    }

    struct timespec delay = {
        .tv_sec = delay_ms / 1000,
        .tv_nsec = (delay_ms % 1000) * 1000000L,
    };

    nanosleep(&delay, NULL);
}

int main(int argc, char **argv) {
    long max_ticks = 30;
    long delay_ms = 1000;
    unsigned int seed = (unsigned int)time(NULL);

    if (argc > 1) {
        max_ticks = parse_nonnegative_long(argv[1], "ticks");
    }

    if (argc > 2) {
        delay_ms = parse_nonnegative_long(argv[2], "delay_ms");
    }

    if (argc > 3) {
        seed = (unsigned int)parse_nonnegative_long(argv[3], "seed");
    }

    if (argc > 4) {
        fprintf(stderr, "Usage: %s [ticks] [delay_ms] [seed]\n", argv[0]);
        return 2;
    }

    signal(SIGINT, on_sigint);
    srand(seed);

    WaterSystem system;
    Dispatcher dispatcher;

    ModelInit(&system);
    DispatcherInit(&dispatcher, &system);

    printf("Городская система водоснабжения\n");
    printf("ticks = %ld (0 = unlimited), delay = %ld ms, random seed = %u\n\n", max_ticks, delay_ms, seed);

    int tick = 0;

    while (!stop_requested && !system.fatal_accident && (max_ticks == 0 || tick < max_ticks)) {
        PrepareTick(&system, tick);
        DispatcherReceiveMessages(&system, &dispatcher, tick);
        DispatcherControl(&system, &dispatcher, tick);
        PhysicalStep(&system, tick);

        if (system.fatal_accident) {
            break;
        }

        GenerateMessages(&system, tick);
        DispatcherReceiveMessages(&system, &dispatcher, tick);
        PrintStatus(&system, &dispatcher, tick);

        ++tick;
        sleep_ms(delay_ms);
    }

    if (stop_requested) {
        printf("[FINISH] Interrupted by user (Ctrl + C)\n");
    } else if (system.fatal_accident) {
        printf("[FINISH] Simulation stopped because of a fatal accident!\n");
    } else {
        printf("[FINISH] Requested tick count is completed\n");
    }

    return system.fatal_accident ? 1 : 0;
}
