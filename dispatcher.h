#ifndef DISPATCHER_H
#define DISPATCHER_H

#include "model.h"

#define PROCESSED_MESSAGE_HISTORY 128

// Знание диспетчера о текущей ситуации
typedef struct {
    bool has_level[RESERVOIR_COUNT]; // имеется ли информация про уровни в резервуарах
    double known_level[RESERVOIR_COUNT]; // информация про уровни в резервуарах
    int known_measure_tick[RESERVOIR_COUNT]; // на каком такте было сделано измерение
    int known_message_id[RESERVOIR_COUNT]; // id сообщения, из которого известно значение

    int processed_ids[PROCESSED_MESSAGE_HISTORY]; // массив обработанных сообщений
    int processed_count; // сколько id сейчас записано в массив
    int processed_next; // позиция для следующего id (нужна, т.к у массива ограниченный размер)
} Dispatcher;

void DispatcherInit(Dispatcher *dispatcher, const WaterSystem *system); // начальная настройка диспетчера
void DispatcherReceiveMessages(WaterSystem *system, Dispatcher *dispatcher, int tick); // функция получения всех сообщений к текущему такту
void DispatcherControl(WaterSystem *system, const Dispatcher *dispatcher, int tick); // функция принятия решений о насосах

#endif
