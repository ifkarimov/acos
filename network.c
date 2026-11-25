#include "network.h"
#include "model.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

volatile sig_atomic_t network_stop = 0;
const char *network_roles[NETWORK_ROLES] = {"reservoir", "district", "pump", "sensor", "dispatcher"};
const int network_counts[NETWORK_ROLES] = {RESERVOIR_COUNT, DISTRICT_COUNT, PUMP_COUNT, RESERVOIR_COUNT, 1};

static void on_signal(int signal_number) {
    network_stop = signal_number;
}

int NetworkSignals(void) {
    struct sigaction action = {0};
    action.sa_handler = on_signal;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) != 0 || sigaction(SIGTERM, &action, NULL) != 0) {
        return -1;
    }
    action.sa_handler = SIG_IGN;
    return sigaction(SIGPIPE, &action, NULL);
}

long NetworkNumber(const char *text, const char *name, long maximum) {
    char *end = NULL;
    errno = 0;
    long value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value < 0 || value > maximum) {
        fprintf(stderr, "Invalid %s: %s\n", name, text);
        exit(2);
    }
    return value;
}

long long NetworkTime(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        perror("clock_gettime");
        exit(2);
    }
    return (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

int NetworkRole(const char *name) {
    for (int i = 0; i < NETWORK_ROLES; ++i) {
        if (strcmp(name, network_roles[i]) == 0) {
            return i;
        }
    }
    return -1;
}

int NetworkIndex(int role, int id) {
    if (role < 0 || role >= NETWORK_ROLES || id < 0 || id >= network_counts[role]) {
        return -1;
    }
    for (int i = 0; i < role; ++i) {
        id += network_counts[i];
    }
    return id;
}

int NetworkSocket(int fd) {
    int flags = fcntl(fd, F_GETFL);
    int enabled = 1;
    if (flags == -1 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1 ||
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled)) == -1) {
        return -1;
    }
    return 0;
}

static int address(const char *ip, int port, struct sockaddr_in *result) {
    memset(result, 0, sizeof(*result));
    result->sin_family = AF_INET;
    result->sin_port = htons((unsigned short)port);
    if (port == 0 || inet_pton(AF_INET, ip, &result->sin_addr) != 1) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

int NetworkListen(const char *ip, int port) {
    struct sockaddr_in local;
    if (address(ip, port, &local) != 0) {
        return -1;
    }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1) {
        return -1;
    }
    int enabled = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) != 0 ||
        NetworkSocket(fd) != 0 || bind(fd, (struct sockaddr *)&local, sizeof(local)) != 0 ||
        listen(fd, NETWORK_CLIENTS) != 0) {
        int error = errno;
        close(fd);
        errno = error;
        return -1;
    }
    return fd;
}

int NetworkConnect(const char *ip, int port) {
    struct sockaddr_in remote;
    if (address(ip, port, &remote) != 0) {
        return -1;
    }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1) {
        return -1;
    }
    if (NetworkSocket(fd) != 0) {
        close(fd);
        return -1;
    }
    int result = connect(fd, (struct sockaddr *)&remote, sizeof(remote));
    if (result == 0) {
        return fd;
    }
    if (errno == EINPROGRESS) {
        long long deadline = NetworkTime() + 5000;
        while (!network_stop && NetworkTime() < deadline) {
            struct pollfd event = {.fd = fd, .events = POLLOUT};
            result = poll(&event, 1, 50);
            if (result == -1 && errno != EINTR) {
                break;
            }
            if (result > 0) {
                int error = 0;
                socklen_t size = sizeof(error);
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) == 0 && error == 0) {
                    return fd;
                }
                errno = error == 0 ? EIO : error;
                break;
            }
            errno = ETIMEDOUT;
        }
    }
    int error = errno;
    close(fd);
    errno = error;
    return -1;
}

int NetworkWrite(int fd, const char *text, size_t size) {
    while (size != 0) {
        ssize_t written = write(fd, text, size);
        if (written > 0) {
            text += written;
            size -= (size_t)written;
        } else if (written == -1 && errno == EINTR && !network_stop) {
            continue;
        } else {
            return -1;
        }
    }
    return 0;
}

// Команды короткие, но даже их send может передать не целиком
int NetworkSend(int fd, const char *text) {
    size_t size = strlen(text);
    long long deadline = NetworkTime() + 3000;
    while (size != 0 && !network_stop && NetworkTime() < deadline) {
        ssize_t sent = send(fd, text, size, 0);
        if (sent > 0) {
            text += sent;
            size -= (size_t)sent;
        } else if (sent == -1 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            struct pollfd event = {.fd = fd, .events = POLLOUT};
            if (poll(&event, 1, 50) == -1 && errno != EINTR) {
                return -1;
            }
        } else {
            return -1;
        }
    }
    return size == 0 ? 0 : -1;
}
