#include "network.h"
#include "model.h"

#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct {
    int fd;
    int role;
    long period;
    long long next_step;
    bool ready;
    bool running;
    bool pending;
    const char *admin_command;
} ClientState;

// Служебные строки начинаются с @, обычные строки содержат события модели
static int receive_line(ClientState *state, const char *line) {
    char extra;
    if (strncmp(line, "@ready ", 7) == 0) {
        long period;
        if (state->ready || sscanf(line, "@ready %ld %c", &period, &extra) != 1 || period < 1 || period > 3600000) {
            return 2;
        }
        state->ready = true;
        state->period = period * (state->role == 3 ? MESSAGE_PERIOD : 1);
        if (state->admin_command != NULL && NetworkSend(state->fd, state->admin_command) != 0) {
            return 2;
        }
    } else if (strncmp(line, "@run ", 5) == 0) {
        long wait;
        if (!state->ready || state->role < 0 || sscanf(line, "@run %ld %c", &wait, &extra) != 1 ||
            wait < 0 || wait > 3600000L * MESSAGE_PERIOD) {
            return 2;
        }
        state->running = true;
        state->pending = false;
        state->next_step = NetworkTime() + wait;
    } else if (strcmp(line, "@pause") == 0) {
        state->running = false;
    } else if (strcmp(line, "@step") == 0) {
        state->pending = false;
        state->next_step = NetworkTime() + state->period;
    } else if (strcmp(line, "@pong") == 0) {
        return -1;
    } else if (strncmp(line, "@done ", 6) == 0) {
        int result;
        if (sscanf(line, "@done %d %c", &result, &extra) != 1 || result < 0 || result > 2) {
            return 2;
        }
        return result;
    } else if (strncmp(line, "@error ", 7) == 0) {
        fprintf(stderr, "%s\n", line + 7);
        return 2;
    } else if (line[0] == '@') {
        fprintf(stderr, "Unknown server response\n");
        return 2;
    } else {
        if (NetworkWrite(STDOUT_FILENO, line, strlen(line)) != 0 || NetworkWrite(STDOUT_FILENO, "\n", 1) != 0) {
            return 2;
        }
    }
    return -1;
}

static int run(ClientState *state) {
    char input[NETWORK_LINE];
    size_t size = 0;
    long long last_reply = NetworkTime();
    long long next_ping = last_reply + 1000;
    while (!network_stop) {
        long long now = NetworkTime();
        if (now - last_reply >= 10000) {
            fprintf(stderr, "Server stopped responding\n");
            return 2;
        }
        if (state->ready && now >= next_ping) {
            if (NetworkSend(state->fd, "PING\n") != 0) {
                return 2;
            }
            next_ping = now + 1000;
        }
        if (state->running && !state->pending && now >= state->next_step) {
            if (NetworkSend(state->fd, "STEP\n") != 0) {
                return 2;
            }
            state->pending = true;
        }
        int timeout = 20;
        if (state->running && !state->pending && state->next_step - now < timeout) {
            timeout = state->next_step > now ? (int)(state->next_step - now) : 0;
        }
        struct pollfd event = {.fd = state->fd, .events = POLLIN};
        int ready = poll(&event, 1, timeout);
        if (ready == -1 && errno == EINTR) {
            continue;
        }
        if (ready == -1 || (event.revents & POLLNVAL) ||
            ((event.revents & POLLERR) && !(event.revents & (POLLIN | POLLHUP)))) {
            fprintf(stderr, "TCP connection failed\n");
            return 2;
        }
        if (!(event.revents & (POLLIN | POLLHUP))) {
            continue;
        }
        ssize_t received = recv(state->fd, input + size, sizeof(input) - 1 - size, 0);
        if (received == -1 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            continue;
        }
        if (received <= 0) {
            fprintf(stderr, "Server closed the connection without a completion message\n");
            return 2;
        }
        if (memchr(input + size, '\0', (size_t)received) != NULL) {
            return 2;
        }
        size += (size_t)received;
        input[size] = '\0';
        char *newline;
        while ((newline = memchr(input, '\n', size)) != NULL) {
            *newline = '\0';
            size_t used = (size_t)(newline - input) + 1;
            last_reply = NetworkTime();
            int result = receive_line(state, input);
            if (result >= 0) {
                return result;
            }
            memmove(input, input + used, size - used);
            size -= used;
        }
        if (size == sizeof(input) - 1) {
            fprintf(stderr, "Server response is too long\n");
            return 2;
        }
    }
    // Закрытие TCP-соединения освобождает роль, остальные участники остаются подключены
    return 0;
}

int NetworkClient(int argc, char **argv, const char *mode) {
    bool worker = strcmp(mode, "worker") == 0;
    bool admin = strcmp(mode, "admin") == 0;
    if ((worker && argc != 5) || (!worker && !admin && argc != 3) ||
        (admin && argc != 4 && argc != 6)) {
        fprintf(stderr, worker ? "Usage: %s ip port role id\n" : admin ?
                "Usage: %s ip port list|stop|disconnect role id|connect role id\n" : "Usage: %s ip port\n", argv[0]);
        return 2;
    }
    int role = worker ? NetworkRole(argv[3]) : -1;
    int id = worker ? (int)NetworkNumber(argv[4], "id", 100) : 0;
    if (worker && NetworkIndex(role, id) < 0) {
        fprintf(stderr, "Invalid role or id\n");
        return 2;
    }
    char admin_command[128] = {0};
    if (admin) {
        if (argc == 4 && strcmp(argv[3], "list") == 0) {
            snprintf(admin_command, sizeof(admin_command), "LIST\n");
        } else if (argc == 4 && strcmp(argv[3], "stop") == 0) {
            snprintf(admin_command, sizeof(admin_command), "STOP\n");
        } else if (argc == 6 && (strcmp(argv[3], "connect") == 0 || strcmp(argv[3], "disconnect") == 0)) {
            int target = (int)NetworkNumber(argv[5], "id", 100);
            bool observer = strcmp(argv[4], "observer") == 0 && strcmp(argv[3], "disconnect") == 0 && target < NETWORK_CLIENTS;
            if (!observer && NetworkIndex(NetworkRole(argv[4]), target) < 0) {
                fprintf(stderr, "Invalid role or id\n");
                return 2;
            }
            snprintf(admin_command, sizeof(admin_command), "%s %s %d\n", strcmp(argv[3], "connect") == 0 ? "CONNECT" : "DISCONNECT", argv[4], target);
        } else {
            fprintf(stderr, "Invalid admin command\n");
            return 2;
        }
    }
    int port = (int)NetworkNumber(argv[2], "port", 65535);
    if (NetworkSignals() != 0) {
        perror("sigaction");
        return 2;
    }
    int fd = NetworkConnect(argv[1], port);
    if (fd == -1) {
        perror("connect");
        return network_stop ? 0 : 2;
    }
    char hello[128];
    snprintf(hello, sizeof(hello), "HELLO 1 %s %d\n", worker ? argv[3] : mode, id);
    ClientState state = {.fd = fd, .role = role, .admin_command = admin ? admin_command : NULL};
    int result = NetworkSend(fd, hello) == 0 ? run(&state) : 2;
    close(fd);
    return network_stop ? 0 : result;
}
