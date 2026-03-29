#include "app_state.h"

// -----------------------------------------------------------------------------
// app_state.cpp
// Файл содержит определения глобальных переменных, объявленных в `include/app_state.h`.
// Разнесение extern-объявлений и определений помогает избежать конфликтов линковки.
// -----------------------------------------------------------------------------

// SerialBT: объект BluetoothSerial (инициализируется в main.cpp через SerialBT.begin()).
BluetoothSerial SerialBT;

// physicsTaskHandle: хендл задачи physicsTask (создаётся в main.cpp).
TaskHandle_t physicsTaskHandle = NULL;

// bluetoothTaskHandle: хендл задачи bluetoothTask (создаётся в main.cpp).
TaskHandle_t bluetoothTaskHandle = NULL;

// connectionTaskHandle: хендл задачи connectionTask (создаётся в main.cpp).
TaskHandle_t connectionTaskHandle = NULL;

// deviceSettings: настройки авто/модели топлива (заполняются из входящих JSON settings).
Settings deviceSettings;

// currentState: текущие вычисленные телеметрические данные (обновляются в physicsTask).
CarState currentState;

// speedFilter: фильтр для сглаживания вычисленной скорости из АЦП.
AdaptiveFilter speedFilter;

// rpmFilter: фильтр для сглаживания RPM.
SimpleFilter rpmFilter;

// voltageFilter: фильтр для сглаживания voltage (в текущем симуляторе voltageFilter не используется).
SimpleFilter voltageFilter;

// lastSentSpeed: последняя отправленная скорость (пороговая логика отправки).
float lastSentSpeed = -1.0f;

// lastSentRPM: последняя отправленная RPM.
float lastSentRPM = -1.0f;

// lastSentFuel: последняя отправленная величина топлива (fuel_level).
float lastSentFuel = -1.0f;

// lastSendTime: millis() последней отправки телеметрии data.
unsigned long lastSendTime = 0;

// lastKeepAliveTime: millis() времени последней keep-alive отправки.
unsigned long lastKeepAliveTime = 0;

// lastPhysicsCalcTime: поле "в запасе" для возможного использования (сейчас не применяется в protocol_json.cpp).
unsigned long lastPhysicsCalcTime = 0;

// forceSendData: принудительный флаг отправки телеметрии (сбрасывается после отправки).
volatile bool forceSendData = false;

// isBluetoothConnected: клиент подключен/не подключен.
volatile bool isBluetoothConnected = false;

// isStreamingActive: активен режим стриминга (после GET_DATA).
volatile bool isStreamingActive = false;

// dataMutex: mutex для синхронизации доступа к shared-state.
SemaphoreHandle_t dataMutex = NULL;

// currentProgramState: текущее состояние протокола/логики.
volatile ProgramState currentProgramState = STATE_WAITING_CONNECTION;

// settingsReceived: true после успешного получения settings от Android.
volatile bool settingsReceived = false;
