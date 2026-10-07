#ifndef MODEL_H
#define MODEL_H

#include <stdbool.h>

#define RESERVOIR_COUNT 2
#define DISTRICT_COUNT 3
#define PUMP_COUNT 4
#define PIPE_COUNT 4
#define MESSAGE_QUEUE_CAPACITY 128

// Фактическое состояние резервуара
typedef struct {
    const char *name;
    double capacity; // максимальная вместимость
    double level; // текущий уровень воды
    double previous_level; // уровень на предыдущем такте
    double inflow; // объём воды, поступающий за один такт
    double lower_bound; // нижняя граница уровня воды
    double higher_bound; // верхняя граница уровня воды (<= capacity)
} Reservoir;

// Информация о районе
typedef struct {
    const char *name;
    double base_demand; // базовая потребность, которая будет меняться
    double demand; // потребность на текущем такте
    double received; // объём воды, полученный на текущем такте
    double deficit; // дефицит воды
} District;

// Фактическое состояние насоса
typedef struct {
    const char *name;
    int reservoir_id; // резервуар насоса
    int district_id; // район насоса
    double capacity; // максимальный объём воды, который насос может прокачать за один такт
    double power; // потребляемая мощность
    bool enabled; // включён/выключен
    double flow; // поток воды на текущем такте
    double previous_flow; // поток воды на предыдущем такте
} Pump;

// Информация о трубе
typedef struct {
    const char *name;
    // int pump_id; // насос трубы
    double capacity; // максимальный пропускаемый поток
} Pipe;

// Информация о сообщении
typedef struct {
    bool active; // сообщение ожидает обработки
    int id; // уникальный порядковый номер сообщения
    int reservoir_id; // какой резервуар измерили
    int measured_tick; // на каком такте сделали измерение
    int delivery_tick; // на каком такте сообщение дойдет до диспетчера
    double measured_level; // уровень в резервуаре (результат измерения)
} Message;

// Информация о физическом мире
typedef struct {
    Reservoir reservoirs[RESERVOIR_COUNT];
    District districts[DISTRICT_COUNT];
    Pump pumps[PUMP_COUNT];
    Pipe pipes[PIPE_COUNT];

    Message messages[MESSAGE_QUEUE_CAPACITY];
    int next_message_id; // номер, который получит следующее сообщение

    double power_limit; // лимит потребляемой мощности для насосов
    bool fatal_accident; // произошла ли авария (нужно ли остановить систему)
} WaterSystem;

void ModelInit(WaterSystem *system); // запуск системы
void PrepareTick(WaterSystem *system, int tick); // функция подготовки нового такта
void PhysicalStep(WaterSystem *system, int tick); // функция физического изменения системы
void GenerateMessages(WaterSystem *system, int tick); // функция создания сообщений датчиков

double ActivePumpPower(const WaterSystem *system); // подсчёт текущей потребляемой мощности

#endif
