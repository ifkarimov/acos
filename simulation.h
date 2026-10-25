#ifndef SIMULATION_H
#define SIMULATION_H

#include "dispatcher.h"
#include "model.h"

#include <pthread.h>
#include <semaphore.h>
#include <signal.h>

#define WORKER_COUNT (RESERVOIR_COUNT * 2 + DISTRICT_COUNT + PUMP_COUNT + 1)

// Общие данные потоков
typedef struct {
    WaterSystem system;
    Dispatcher dispatcher;

    pthread_mutex_t mutex; // защищает состояние модели и диспетчера
    pthread_cond_t condition; // ожидание запуска, следующего шага или завершения
    sem_t *dispatcher_event; // уведомления для диспетчера

    bool started; // все потоки созданы, можно начинать работу
    bool stopping; // нужно завершить все потоки
    bool system_error; // ошибка работы с потоками или временем
    int tick; // число прошедших периодов моделирования
    long delay_ms; // длительность одного периода
} Simulation;

// Данные одного потока
typedef struct {
    Simulation *simulation;
    int id; // номер резервуара, района или насоса
    unsigned int seed; // отдельное состояние генератора для датчика
} ThreadData;

int RunSimulation(int max_ticks, long delay_ms, unsigned int seed, const volatile sig_atomic_t *stop_requested);
// создание потоков, вывод состояния и завершение моделирования

#endif
