#ifndef NETWORK_H
#define NETWORK_H

#include <signal.h>
#include <stddef.h>

#define NETWORK_PARTICIPANTS 12
#define NETWORK_ROLES 5
#define NETWORK_CLIENTS 64
#define NETWORK_LINE 2048
#define NETWORK_LOG 65536
#define NETWORK_QUEUE 262144

extern volatile sig_atomic_t network_stop;
extern const char *network_roles[NETWORK_ROLES];
extern const int network_counts[NETWORK_ROLES];

int NetworkSignals(void);
long NetworkNumber(const char *text, const char *name, long maximum);
long long NetworkTime(void);
int NetworkRole(const char *name);
int NetworkIndex(int role, int id);
int NetworkSocket(int fd);
int NetworkListen(const char *ip, int port);
int NetworkConnect(const char *ip, int port);
int NetworkWrite(int fd, const char *text, size_t size);
int NetworkSend(int fd, const char *text);

// Вывод исходных функций модели собирается в текст одного сетевого ответа
int NetworkPrint(const char *format, ...);
void NetworkClearLog(void);
const char *NetworkGetLog(void);
int NetworkLogFailed(void);

int NetworkClient(int argc, char **argv, const char *mode);

#endif
