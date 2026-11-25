#include "network.h"

#include <stdarg.h>
#include <stdio.h>

static char log_text[NETWORK_LOG];
static size_t log_size;
static int log_failed;

void NetworkClearLog(void) {
    log_size = 0;
    log_text[0] = '\0';
    log_failed = 0;
}

const char *NetworkGetLog(void) {
    return log_text;
}

int NetworkLogFailed(void) {
    return log_failed;
}

// Только сетевые сборки model.c, dispatcher.c и output.c вызывают эту функцию вместо printf
int NetworkPrint(const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    int written = vsnprintf(log_text + log_size, sizeof(log_text) - log_size, format, arguments);
    va_end(arguments);
    if (written < 0 || (size_t)written >= sizeof(log_text) - log_size) {
        log_failed = 1;
        return -1;
    }
    log_size += (size_t)written;
    return written;
}
