#ifndef IPC_H
#define IPC_H

#include "simulation.h"

Simulation *CreateSharedSimulation(void); // разделяемая память и семафоры
bool DestroySharedSimulation(Simulation *simulation); // вызывается после завершения дочерних процессов

#endif
