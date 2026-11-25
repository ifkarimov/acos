#ifndef SIMULATION_H
#define SIMULATION_H

#include "dispatcher.h"
#include "model.h"

#include <semaphore.h>
#include <signal.h>

#define WORKER_COUNT (RESERVOIR_COUNT * 2 + DISTRICT_COUNT + PUMP_COUNT + 1)

// Общие данные процессов в разделяемой памяти POSIX
typedef struct {
    WaterSystem system;
    Dispatcher dispatcher;

    sem_t *mutex; // защищает состояние модели и диспетчера
    sem_t *start_event; // разрешение запуска дочерних процессов
    sem_t *stop_event[WORKER_COUNT]; // отдельное ожидание для каждого процесса
    sem_t semaphores[WORKER_COUNT + 3]; // память для неименованного варианта
    sem_t *dispatcher_event; // уведомления для диспетчера

    bool started; // все процессы созданы, можно начинать работу
    bool stopping; // нужно завершить все процессы
    bool system_error; // ошибка работы с процессами или временем
    int tick; // число прошедших периодов моделирования
    long delay_ms; // длительность одного периода
} Simulation;

// Данные одного процесса
typedef struct {
    Simulation *simulation;
    int id; // номер резервуара, района или насоса
    unsigned int seed; // отдельное состояние генератора для датчика
    int index; // номер процесса в массиве семафоров ожидания
} ProcessData;

int RunSimulation(int max_ticks, long delay_ms, unsigned int seed, const volatile sig_atomic_t *stop_requested);
// создание процессов, вывод состояния и завершение моделирования

#endif
