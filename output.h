#ifndef OUTPUT_H
#define OUTPUT_H

#include "dispatcher.h"
#include "model.h"

void PrintStatus(const WaterSystem *system, const Dispatcher *dispatcher, int tick);
// функция вывода полного состояния системы на текущем такте

#endif
