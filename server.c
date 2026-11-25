#include "network.h"
#include "dispatcher.h"
#include "output.h"

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define UNREGISTERED -3
#define ADMIN -2
#define OBSERVER -1

typedef struct {
    int fd;
    int role;
    int id;
    bool closing;
    bool write_shutdown;
    long long last_line;
    long long close_deadline;
    char input[NETWORK_LINE];
    size_t input_size;
    char output[NETWORK_QUEUE];
    size_t output_start;
    size_t output_size;
} Client;

typedef struct {
    WaterSystem system;
    Dispatcher dispatcher;
    Client clients[NETWORK_CLIENTS];
    int participants[NETWORK_PARTICIPANTS];
    bool blocked[NETWORK_PARTICIPANTS];
    long long next_step[NETWORK_PARTICIPANTS];
    unsigned int seeds[RESERVOIR_COUNT];
    int listener;
    int tick;
    int max_ticks;
    long period;
    long long next_tick;
    long long paused_at;
    long long startup_deadline;
    bool started;
    bool running;
    bool stopping;
    int result;
} Server;

static void release_role(Server *server, int slot) {
    Client *client = &server->clients[slot];
    int index = NetworkIndex(client->role, client->id);
    if (index >= 0 && server->participants[index] == slot) {
        server->participants[index] = -1;
    }
}

static void close_client(Server *server, int slot) {
    Client *client = &server->clients[slot];
    if (client->fd != -1) {
        release_role(server, slot);
        close(client->fd);
        client->fd = -1;
    }
}

static bool queue_text(Server *server, int slot, const char *text) {
    Client *client = &server->clients[slot];
    if (client->fd == -1) {
        return false;
    }
    size_t size = strlen(text);
    size_t remaining = client->output_size - client->output_start;
    if (size > sizeof(client->output) - remaining) {
        // Медленный наблюдатель не должен останавливать остальных
        fprintf(stderr, "Client %d cannot receive output fast enough\n", slot);
        close_client(server, slot);
        return false;
    }
    if (client->output_start != 0) {
        memmove(client->output, client->output + client->output_start, remaining);
        client->output_start = 0;
        client->output_size = remaining;
    }
    memcpy(client->output + client->output_size, text, size);
    client->output_size += size;
    return true;
}

static void observers(Server *server, const char *text) {
    for (int i = 0; i < NETWORK_CLIENTS; ++i) {
        if (server->clients[i].fd != -1 && server->clients[i].role == OBSERVER && !server->clients[i].closing) {
            queue_text(server, i, text);
        }
    }
}

static void notice(Server *server, const char *text) {
    if (NetworkWrite(STDOUT_FILENO, text, strlen(text)) != 0) {
        server->result = 2;
    }
    for (int i = 0; i < NETWORK_CLIENTS; ++i) {
        if (server->clients[i].fd != -1 && server->clients[i].role != UNREGISTERED && !server->clients[i].closing) {
            queue_text(server, i, text);
        }
    }
}

static void retire_client(Server *server, int slot, const char *message) {
    Client *client = &server->clients[slot];
    queue_text(server, slot, message);
    release_role(server, slot);
    client->closing = true;
    client->close_deadline = NetworkTime() + 2000;
}

static void status(Server *server, int slot, bool summary) {
    NetworkClearLog();
    PrintStatus(&server->system, &server->dispatcher, server->tick);
    if (summary) {
        PrintSummary(&server->system);
    }
    NetworkPrint("[STATE] %s\n", server->running ? "running" : server->stopping ? "finished" : "waiting for participants");
    for (int role = 0; role < NETWORK_ROLES; ++role) {
        for (int id = 0; id < network_counts[role]; ++id) {
            int index = NetworkIndex(role, id);
            NetworkPrint("[PARTICIPANT] %s %d: %s\n", network_roles[role], id,
                         server->blocked[index] ? "blocked" : server->participants[index] >= 0 ? "connected" : "absent");
        }
    }
    if (NetworkLogFailed()) {
        server->result = 2;
    } else if (slot >= 0) {
        queue_text(server, slot, NetworkGetLog());
    } else {
        observers(server, NetworkGetLog());
    }
}

static void finish(Server *server, int result, const char *reason) {
    if (server->stopping) {
        return;
    }
    server->stopping = true;
    server->running = false;
    server->result = result;
    close(server->listener);
    server->listener = -1;
    char text[256];
    snprintf(text, sizeof(text), "[FINISH] %s\n", reason);
    notice(server, text);
    status(server, -1, true);
    snprintf(text, sizeof(text), "@done %d\n", server->result);
    for (int i = 0; i < NETWORK_CLIENTS; ++i) {
        if (server->clients[i].fd != -1) {
            retire_client(server, i, text);
        }
    }
}

// Все изменения модели выполняет один цикл сервера, блокировка между компьютерами не нужна
static void step(Server *server, int slot) {
    Client *client = &server->clients[slot];
    int index = NetworkIndex(client->role, client->id);
    if (index < 0) {
        retire_client(server, slot, "@error Only participants can request a step\n");
        return;
    }
    long long now = NetworkTime();
    if (!server->running || now < server->next_step[index]) {
        queue_text(server, slot, "@step\n");
        return;
    }
    NetworkClearLog();
    NetworkPrint("[STEP][Tick #%d] %s %d\n", server->tick, network_roles[client->role], client->id);
    if (client->role == 0) {
        ReservoirStep(&server->system, client->id, server->tick);
    } else if (client->role == 1) {
        DistrictFinish(&server->system, client->id, server->tick);
        DistrictPrepare(&server->system, client->id, server->tick);
    } else if (client->role == 2) {
        PumpStep(&server->system, client->id, server->tick);
    } else if (client->role == 3) {
        GenerateMessage(&server->system, client->id, server->tick, &server->seeds[client->id]);
    } else {
        DispatcherReceiveMessages(&server->system, &server->dispatcher, server->tick);
        DispatcherControl(&server->system, &server->dispatcher, server->tick);
    }
    long period = server->period * (client->role == 3 ? MESSAGE_PERIOD : 1);
    server->next_step[index] = now + period;
    if (NetworkLogFailed() || NetworkWrite(STDOUT_FILENO, NetworkGetLog(), strlen(NetworkGetLog())) != 0) {
        server->result = 2;
        return;
    }
    queue_text(server, slot, NetworkGetLog());
    observers(server, NetworkGetLog());
    queue_text(server, slot, "@step\n");
    if (server->system.fatal_accident) {
        finish(server, 1, "Fatal accident");
    }
}

static void update_running(Server *server) {
    if (server->stopping) {
        return;
    }
    int connected = 0;
    for (int i = 0; i < NETWORK_PARTICIPANTS; ++i) {
        connected += server->participants[i] >= 0;
    }
    if (connected != NETWORK_PARTICIPANTS && server->running) {
        server->running = false;
        server->paused_at = NetworkTime();
        notice(server, "[PAUSE] Waiting for missing participants, model state is saved\n");
        for (int i = 0; i < NETWORK_CLIENTS; ++i) {
            if (server->clients[i].fd != -1 && server->clients[i].role >= 0 && !server->clients[i].closing) {
                queue_text(server, i, "@pause\n");
            }
        }
    } else if (connected == NETWORK_PARTICIPANTS && !server->running) {
        if (!server->started) {
            for (int id = 0; id < DISTRICT_COUNT; ++id) {
                NetworkClearLog();
                DistrictPrepare(&server->system, id, server->tick);
                queue_text(server, server->participants[NetworkIndex(1, id)], NetworkGetLog());
                observers(server, NetworkGetLog());
            }
        }
        bool resumed = server->started;
        server->started = true;
        server->running = true;
        long long now = NetworkTime();
        long long paused = resumed ? now - server->paused_at : 0;
        server->next_tick = resumed ? server->next_tick + paused : now + server->period;
        notice(server, resumed ? "[RESUME] All participants are connected\n" : "[START] Connected 12 participants\n");
        for (int role = 0; role < NETWORK_ROLES; ++role) {
            for (int id = 0; id < network_counts[role]; ++id) {
                int index = NetworkIndex(role, id);
                server->next_step[index] = resumed ? server->next_step[index] + paused : now + (role == 1 ? server->period : 0);
                long long wait = server->next_step[index] > now ? server->next_step[index] - now : 0;
                char text[64];
                snprintf(text, sizeof(text), "@run %lld\n", wait);
                queue_text(server, server->participants[index], text);
            }
        }
    }
}

static void register_client(Server *server, int slot, const char *line) {
    char version[16], name[32], number[32], extra;
    if (sscanf(line, "HELLO %15s %31s %31s %c", version, name, number, &extra) != 3 || strcmp(version, "1") != 0) {
        retire_client(server, slot, "@error Expected HELLO 1 role id\n");
        return;
    }
    char *end;
    errno = 0;
    long id = strtol(number, &end, 10);
    if (errno != 0 || end == number || *end != '\0' || id < 0 || id > INT_MAX) {
        retire_client(server, slot, "@error Invalid participant id\n");
        return;
    }
    int role = NetworkRole(name);
    if (strcmp(name, "observer") == 0 && id == 0) {
        role = OBSERVER;
    } else if (strcmp(name, "admin") == 0 && id == 0) {
        role = ADMIN;
    } else {
        int index = NetworkIndex(role, (int)id);
        if (index < 0 || server->blocked[index] || server->participants[index] >= 0) {
            retire_client(server, slot, "@error Unknown, blocked or occupied participant\n");
            return;
        }
        server->participants[index] = slot;
    }
    Client *client = &server->clients[slot];
    client->role = role;
    client->id = (int)id;
    char text[256];
    snprintf(text, sizeof(text), "@ready %ld\n[REGISTER] %s %ld: connection = %d\n", server->period, name, id, slot);
    queue_text(server, slot, text);
    snprintf(text, sizeof(text), "[CONNECT] %s %ld: connection = %d\n", name, id, slot);
    if (NetworkWrite(STDOUT_FILENO, text, strlen(text)) != 0) {
        server->result = 2;
    }
    observers(server, text);
    if (role == OBSERVER) {
        status(server, slot, false);
    }
    update_running(server);
}

static void admin_command(Server *server, int slot, const char *line) {
    if (strcmp(line, "STOP") == 0) {
        finish(server, 0, "Stopped by administrator");
        return;
    }
    if (strcmp(line, "LIST") == 0) {
        char text[128];
        for (int role = 0; role < NETWORK_ROLES; ++role) {
            for (int id = 0; id < network_counts[role]; ++id) {
                int index = NetworkIndex(role, id);
                snprintf(text, sizeof(text), "%s %d: %s\n", network_roles[role], id,
                         server->blocked[index] ? "blocked" : server->participants[index] >= 0 ? "connected" : "absent");
                queue_text(server, slot, text);
            }
        }
        for (int i = 0; i < NETWORK_CLIENTS; ++i) {
            if (server->clients[i].fd != -1 && server->clients[i].role == OBSERVER && !server->clients[i].closing) {
                snprintf(text, sizeof(text), "observer %d: connected\n", i);
                queue_text(server, slot, text);
            }
        }
        retire_client(server, slot, "@done 0\n");
        return;
    }
    char command[16], name[32], number[32], extra;
    if (sscanf(line, "%15s %31s %31s %c", command, name, number, &extra) != 3 ||
        (strcmp(command, "CONNECT") != 0 && strcmp(command, "DISCONNECT") != 0)) {
        retire_client(server, slot, "@error Unknown admin command\n");
        return;
    }
    char *end;
    errno = 0;
    long id = strtol(number, &end, 10);
    if (errno != 0 || end == number || *end != '\0' || id < 0 || id > INT_MAX) {
        retire_client(server, slot, "@error Invalid participant id\n");
        return;
    }
    bool disconnect = strcmp(command, "DISCONNECT") == 0;
    int index = NetworkIndex(NetworkRole(name), (int)id);
    if (strcmp(name, "observer") == 0 && disconnect && id < NETWORK_CLIENTS &&
        server->clients[id].fd != -1 && server->clients[id].role == OBSERVER) {
        retire_client(server, (int)id, "[DISCONNECT] Disconnected by administrator\n@done 0\n");
    } else if (index >= 0) {
        server->blocked[index] = disconnect;
        if (disconnect && server->participants[index] >= 0) {
            retire_client(server, server->participants[index], "[DISCONNECT] Disconnected by administrator\n@done 0\n");
        }
    } else {
        retire_client(server, slot, "@error Unknown participant\n");
        return;
    }
    char text[256];
    snprintf(text, sizeof(text), "[ADMIN] %s %s %ld\n", command, name, id);
    notice(server, text);
    retire_client(server, slot, "@done 0\n");
    update_running(server);
}

static void command(Server *server, int slot, const char *line) {
    Client *client = &server->clients[slot];
    if (client->role == UNREGISTERED) {
        register_client(server, slot, line);
    } else if (strcmp(line, "PING") == 0) {
        queue_text(server, slot, "@pong\n");
    } else if (strcmp(line, "QUIT") == 0) {
        retire_client(server, slot, "@done 0\n");
    } else if (client->role == ADMIN) {
        admin_command(server, slot, line);
    } else if (strcmp(line, "STEP") == 0) {
        update_running(server);
        step(server, slot);
    } else {
        retire_client(server, slot, "@error Unknown command\n");
    }
}

static void read_client(Server *server, int slot) {
    Client *client = &server->clients[slot];
    ssize_t received = recv(client->fd, client->input + client->input_size,
                            sizeof(client->input) - 1 - client->input_size, 0);
    if (received == 0 || (received == -1 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
        close_client(server, slot);
        return;
    }
    if (received < 0) {
        return;
    }
    if (memchr(client->input + client->input_size, '\0', (size_t)received) != NULL) {
        retire_client(server, slot, "@error Unexpected zero byte\n");
        return;
    }
    client->input_size += (size_t)received;
    client->input[client->input_size] = '\0';
    char *newline;
    while ((newline = memchr(client->input, '\n', client->input_size)) != NULL &&
           client->fd != -1 && !client->closing && !server->stopping) {
        *newline = '\0';
        size_t used = (size_t)(newline - client->input) + 1;
        client->last_line = NetworkTime();
        command(server, slot, client->input);
        memmove(client->input, client->input + used, client->input_size - used);
        client->input_size -= used;
    }
    if (client->input_size == sizeof(client->input) - 1 && !client->closing) {
        retire_client(server, slot, "@error Command is too long\n");
    }
}

static void write_client(Server *server, int slot) {
    Client *client = &server->clients[slot];
    size_t remaining = client->output_size - client->output_start;
    if (remaining == 0) {
        return;
    }
    if (remaining > NETWORK_LOG) {
        remaining = NETWORK_LOG;
    }
    ssize_t sent = send(client->fd, client->output + client->output_start, remaining, 0);
    if (sent > 0) {
        client->output_start += (size_t)sent;
        if (client->output_start == client->output_size) {
            client->output_start = 0;
            client->output_size = 0;
        }
    } else if (sent == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
        close_client(server, slot);
    }
}

static void accept_clients(Server *server) {
    for (int attempt = 0; attempt < 16; ++attempt) {
        int fd = accept(server->listener, NULL, NULL);
        if (fd == -1) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                perror("accept");
                server->result = 2;
            }
            return;
        }
        int slot = 0;
        while (slot < NETWORK_CLIENTS && server->clients[slot].fd != -1) {
            ++slot;
        }
        if (slot == NETWORK_CLIENTS || NetworkSocket(fd) != 0) {
            close(fd);
            continue;
        }
        Client *client = &server->clients[slot];
        memset(client, 0, sizeof(*client));
        client->fd = fd;
        client->role = UNREGISTERED;
        client->last_line = NetworkTime();
    }
}

static int run(Server *server) {
    while (true) {
        long long now = NetworkTime();
        if (!server->stopping) {
            update_running(server);
            if (server->result == 2) {
                finish(server, 2, "System error");
            } else if (network_stop) {
                finish(server, 0, "Interrupted by user");
            } else if (!server->started && now >= server->startup_deadline) {
                finish(server, 2, "Not all participants connected within 60 seconds");
            } else if (server->running && now >= server->next_tick) {
                status(server, -1, false);
                if ((server->max_ticks != 0 && server->tick >= server->max_ticks) ||
                    server->tick >= INT_MAX - 2 * MAX_MESSAGE_DELAY) {
                    finish(server, 0, "Requested tick count is completed");
                } else {
                    ++server->tick;
                    server->next_tick = now + server->period;
                }
            }
        }
        struct pollfd events[NETWORK_CLIENTS + 1];
        events[0] = (struct pollfd){.fd = server->listener, .events = POLLIN};
        int active = 0;
        for (int i = 0; i < NETWORK_CLIENTS; ++i) {
            Client *client = &server->clients[i];
            if (client->fd != -1 && client->closing && now >= client->close_deadline) {
                close_client(server, i);
            }
            if (client->fd != -1 && client->closing && !client->write_shutdown &&
                client->output_size == client->output_start) {
                // Сначала передаём итог, затем дочитываем вход, чтобы close не отправил RST
                shutdown(client->fd, SHUT_WR);
                client->write_shutdown = true;
            }
            if (client->fd != -1 && !client->closing && now - client->last_line >= 10000) {
                retire_client(server, i, "@error Client stopped responding\n");
            }
            events[i + 1] = (struct pollfd){.fd = client->fd, .events = POLLIN};
            if (client->output_size != client->output_start) {
                events[i + 1].events |= POLLOUT;
            }
            active += client->fd != -1;
        }
        if (server->stopping && active == 0) {
            return server->result;
        }
        int timeout = 20;
        if (server->running && server->next_tick - now < timeout) {
            timeout = server->next_tick > now ? (int)(server->next_tick - now) : 0;
        }
        int ready = poll(events, NETWORK_CLIENTS + 1, timeout);
        if (ready == -1) {
            if (errno == EINTR) {
                continue;
            }
            perror("poll");
            return 2;
        }
        // Сначала обрабатываем уже подключённых клиентов, затем принимаем новые соединения
        for (int i = 0; i < NETWORK_CLIENTS; ++i) {
            Client *client = &server->clients[i];
            short event = events[i + 1].revents;
            if (client->fd == -1) {
                continue;
            }
            if (event & (POLLERR | POLLNVAL)) {
                close_client(server, i);
                continue;
            }
            if ((event & (POLLIN | POLLHUP)) && !client->closing && !server->stopping) {
                read_client(server, i);
            } else if ((event & (POLLIN | POLLHUP)) && client->closing) {
                char discarded[NETWORK_LINE];
                ssize_t received = recv(client->fd, discarded, sizeof(discarded), 0);
                if (received == 0 || (received == -1 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
                    close_client(server, i);
                }
            }
            if (client->fd != -1 && (event & POLLOUT)) {
                write_client(server, i);
            }
            if (client->fd != -1 && (event & POLLHUP)) {
                close_client(server, i);
            }
            update_running(server);
        }
        if (server->listener != -1 && (events[0].revents & POLLIN)) {
            accept_clients(server);
        }
    }
}

int main(int argc, char **argv) {
    if (argc < 3 || argc > 6) {
        fprintf(stderr, "Usage: %s ip port [ticks] [delay_ms] [seed]\n", argv[0]);
        return 2;
    }
    int port = (int)NetworkNumber(argv[2], "port", 65535);
    int ticks = argc > 3 ? (int)NetworkNumber(argv[3], "ticks", INT_MAX - 2 * MAX_MESSAGE_DELAY) : 30;
    long delay = argc > 4 ? NetworkNumber(argv[4], "delay_ms", 3600000) : 1000;
    unsigned int seed = argc > 5 ? (unsigned int)NetworkNumber(argv[5], "seed", LONG_MAX < UINT_MAX ? LONG_MAX : (long)UINT_MAX) : (unsigned int)time(NULL);
    if (NetworkSignals() != 0) {
        perror("sigaction");
        return 2;
    }
    int listener = NetworkListen(argv[1], port);
    if (listener == -1) {
        perror("listen");
        return 2;
    }
    Server *server = calloc(1, sizeof(*server));
    if (server == NULL) {
        perror("calloc");
        close(listener);
        return 2;
    }
    server->listener = listener;
    server->tick = 1;
    server->max_ticks = ticks;
    server->period = delay == 0 ? 1 : delay;
    server->startup_deadline = NetworkTime() + 60000;
    for (int i = 0; i < NETWORK_CLIENTS; ++i) {
        server->clients[i].fd = -1;
    }
    for (int i = 0; i < NETWORK_PARTICIPANTS; ++i) {
        server->participants[i] = -1;
    }
    for (int i = 0; i < RESERVOIR_COUNT; ++i) {
        server->seeds[i] = seed + (unsigned int)i * 2654435761u;
    }
    ModelInit(&server->system);
    DispatcherInit(&server->dispatcher, &server->system);
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("[READY] TCP server %s:%d, ticks = %d, delay = %ld ms, seed = %u\n", argv[1], port, ticks, delay, seed);
    int result = run(server);
    for (int i = 0; i < NETWORK_CLIENTS; ++i) {
        close_client(server, i);
    }
    if (server->listener != -1) {
        close(server->listener);
    }
    free(server);
    return result;
}
