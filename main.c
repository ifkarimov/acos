#define _POSIX_C_SOURCE 200809L

#include "dispatcher.h"
#include "model.h"
#include "output.h"
#include "simulation.h"

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static volatile sig_atomic_t stop_requested = 0;

static void on_signal(int signal_number) {
    stop_requested = signal_number;
}

static long parse_nonnegative_long(const char *text, const char *name, long maximum) {
    char *end = NULL;
    errno = 0;

    long value = strtol(text, &end, 10);

    if (errno != 0 || end == text || *end != '\0' || value < 0 || value > maximum) {
        fprintf(stderr, "Invalid %s: %s\n", name, text);
        exit(2);
    }

    return value;
}

int main(int argc, char **argv) {
    int max_ticks = 30;
    long delay_ms = 1000;
    unsigned int seed = (unsigned int)time(NULL);

    if (argc > 1) {
        max_ticks = (int)parse_nonnegative_long(argv[1], "ticks", INT_MAX - 2 * MAX_MESSAGE_DELAY);
    }

    if (argc > 2) {
        delay_ms = parse_nonnegative_long(argv[2], "delay_ms", 3600000);
    }

    if (argc > 3) {
        seed = (unsigned int)parse_nonnegative_long(argv[3], "seed", LONG_MAX < UINT_MAX ? LONG_MAX : (long)UINT_MAX);
    }

    if (argc > 4) {
        fprintf(stderr, "Usage: %s [ticks] [delay_ms] [seed]\n", argv[0]);
        return 2;
    }

    struct sigaction action = {0};
    action.sa_handler = on_signal;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) != 0 || sigaction(SIGTERM, &action, NULL) != 0) {
        perror("sigaction");
        return 2;
    }

    // строки сразу попадают в терминал или перенаправленный файл
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("Городская система водоснабжения\n");
    printf("ticks = %d (0 = unlimited), delay = %ld ms (0 = fast mode), random seed = %u\n\n", max_ticks, delay_ms, seed);

    int result = RunSimulation(max_ticks, delay_ms, seed, &stop_requested);
    if (fflush(stdout) == EOF || ferror(stdout)) {
        fprintf(stderr, "Failed to write simulation output\n");
        return 2;
    }

    return result;
}
