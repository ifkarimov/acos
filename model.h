#ifndef MODEL_H
#define MODEL_H

#include <stdbool.h>

#define RESERVOIR_COUNT 2
#define DISTRICT_COUNT 3
#define PUMP_COUNT 4
#define PIPE_COUNT 4
#define MESSAGE_QUEUE_CAPACITY 128
#define MESSAGE_PERIOD 2
#define MESSAGE_LOSS_PERCENT 15
#define MESSAGE_DUPLICATE_PERCENT 15
#define MAX_MESSAGE_DELAY 3

// В ДЗ2 такт в полях объектов означает их собственный шаг или период.
// previous_level хранит уровень до последнего изменения, flow - результат последнего шага насоса.

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
    double total_demand; // суммарная потребность за завершённые периоды
    double total_received; // суммарно полученная вода
    double total_deficit; // суммарный дефицит
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

    unsigned long long measured_count; // сколько измерений сделали датчики
    unsigned long long lost_count; // сколько сообщений потерялось
    unsigned long long duplicate_count; // сколько дубликатов отправили
    unsigned long long received_count; // сколько сообщений дошло до диспетчера
    unsigned long long ignored_duplicate_count; // сколько дубликатов проигнорировали
    unsigned long long ignored_old_count; // сколько устаревших сообщений проигнорировали
} WaterSystem;

// После запуска потоков эти функции вызываются при захваченном мьютексе модели
void ModelInit(WaterSystem *system); // запуск системы
void ReservoirStep(WaterSystem *system, int reservoir_id, int tick); // пополнение резервуара
void DistrictPrepare(WaterSystem *system, int district_id, int tick); // новая потребность района
void DistrictFinish(WaterSystem *system, int district_id, int tick); // результат снабжения района
void PumpStep(WaterSystem *system, int pump_id, int tick); // перекачивание воды
void GenerateMessage(WaterSystem *system, int reservoir_id, int tick, unsigned int *seed); // измерение датчика

double ActivePumpPower(const WaterSystem *system); // подсчёт текущей потребляемой мощности

#endif
