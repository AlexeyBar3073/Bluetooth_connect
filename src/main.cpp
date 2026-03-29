#include <Arduino.h>
#include <ArduinoJson.h>
#include "BluetoothSerial.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <math.h>

// === КОНСТАНТЫ И НАСТРОЙКИ ===
#define POTENTIOMETER_PIN 33     // GPIO33
#define SPEED_MAX 220.0f         // Максимальная скорость, км/ч
#define ADC_MAX 4095.0f          // Максимальное значение АЦП
#define RPM_IDLE 750.0f          // Обороты на холостом ходу
#define RPM_MAX 6500.0f          // Максимальные обороты
#define VOLTAGE_IDLE 11.0f       // Напряжение на холостом ходу 13.3f 
#define INJECTOR_PULSE_MS 7.0f   // Импульс форсунки
#define TASK_STACK_SIZE 8192     // Размер стека задач
#define CONNECTION_DEBOUNCE_MS 500 // Защита от быстрых переподключений

// Параметры отправки данных
const unsigned long MIN_SEND_INTERVAL_MS = 50;
const float SPEED_CHANGE_THRESHOLD = 0.1f;
const float RPM_CHANGE_THRESHOLD = 5.0f;
const float FUEL_CHANGE_THRESHOLD = 0.05f;

// === ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ И СТРУКТУРЫ ===
BluetoothSerial SerialBT;
TaskHandle_t physicsTaskHandle = NULL;
TaskHandle_t bluetoothTaskHandle = NULL;
TaskHandle_t connectionTaskHandle = NULL;

// Настройки от Android
struct Settings {
    float fuel_tank_capacity = 60.0f;
    int injector_count = 4;
    float injector_performance = 250.0f;
    int speed_sensor_signals = 2000;
    float initial_fuel = 60.0f;
} deviceSettings;

// Текущие данные автомобиля
struct CarState {
    float speed = 0.0f;
    float rpm = RPM_IDLE;
    float voltage = VOLTAGE_IDLE;
    float trip_a = 0.0f;
    float trip_b = 0.0f;
    float odometer = 12450.7f;
    float fuel_used_total = 0.0f;
    float fuel_level = 60.0f;
    float fuel_consumption = 0.0f;
    float remaining_range = 0.0f;
    int gear = 1;                 // Текущая передача (4-ступенчатая)
} currentState;

// === АДАПТИВНЫЙ ФИЛЬТР ДЛЯ СКОРОСТИ ===
struct AdaptiveFilter {
    float value = 0;
    bool initialized = false;

    float update(float input) {
        if (!initialized) {
            value = input;
            initialized = true;
            return value;
        }

        float diff = fabs(input - value);

        const float noiseLevel = 0.2f;
        const float fastThreshold = 5.0f;
        const float alphaSlow = 0.05f;
        const float alphaFast = 0.7f;

        float alpha;

        if (diff < noiseLevel) {
            return value;
        }
        else if (diff > fastThreshold) {
            alpha = alphaFast;
        }
        else {
            float t = (diff - noiseLevel) / (fastThreshold - noiseLevel);
            alpha = alphaSlow + t * (alphaFast - alphaSlow);
        }

        value = value + alpha * (input - value);
        return value;
    }

    void reset() {
        initialized = false;
        value = 0;
    }
};

// === ПРОСТОЙ ФИЛЬТР ДЛЯ RPM И НАПРЯЖЕНИЯ ===
struct SimpleFilter {
    float lastValue = 0;
    bool initialized = false;
    const float alpha = 0.3f;

    float update(float input) {
        if (!initialized) {
            lastValue = input;
            initialized = true;
            return input;
        }
        
        lastValue = lastValue * (1 - alpha) + input * alpha;
        return lastValue;
    }

    void reset() {
        initialized = false;
        lastValue = 0;
    }
};

// Глобальные фильтры
AdaptiveFilter speedFilter;
SimpleFilter rpmFilter;
SimpleFilter voltageFilter;

// Переменные для отслеживания изменений
float lastSentSpeed = -1.0f;
float lastSentRPM = -1.0f;
float lastSentFuel = -1.0f;
unsigned long lastSendTime = 0;
unsigned long lastKeepAliveTime = 0;
unsigned long lastPhysicsCalcTime = 0;

volatile bool forceSendData = false;
volatile bool isBluetoothConnected = false;
volatile bool isStreamingActive = false;

SemaphoreHandle_t dataMutex = NULL;

enum ProgramState {
    STATE_WAITING_CONNECTION,
    STATE_WAITING_SETTINGS,
    STATE_IDLE,
    STATE_STREAMING_DATA
};
volatile ProgramState currentProgramState = STATE_WAITING_CONNECTION;
volatile bool settingsReceived = false;

// === ПРОТОТИПЫ ФУНКЦИЙ ===
float calculateRPM(float speed);
float calculateVoltage(float rpm);
float calculateFuelUsage(float rpm, unsigned long delta_ms);
float calculateInstantConsumption(float speed, float fuel_usage_per_sec);
float calculateRemainingRange(float fuel_level, float consumption);
int calculateGear(float speed);
void physicsTask(void *parameter);
void bluetoothTask(void *parameter);
void connectionTask(void *parameter);
void sendJSONResponse(const String& key, const String& value);
void sendCarData();
void processIncomingJSON(const String& jsonString);
void stopAllCalculations();
void startAllCalculations();
void updateFuelLevel();
void setConnectionState(bool connected);
bool getConnectionState();

// === ИСПРАВЛЕННАЯ ФУНКЦИЯ РАСЧЕТА ОБОРОТОВ ===
float calculateRPM(float speed) {
    if (speed <= 0.1f) return RPM_IDLE;
    
    if (speed <= 60.0f) {
        // Маппинг 0-60 км/ч: 750-1500 RPM (коэффициент 12.5)
        return RPM_IDLE + (speed * 12.5f);
    }
    else {
        // Маппинг 60+ км/ч: коэффициент 25 RPM/км/ч
        return 1500.0f + ((speed - 60.0f) * 25.0f);
    }
}

float calculateVoltage(float rpm) {
    /*
    if (rpm < 1000.0f) return VOLTAGE_IDLE;
    else if (rpm < 2500.0f) return VOLTAGE_IDLE + ((rpm - 1000.0f) * 0.7f / 1500.0f);
    else if (rpm < 4000.0f) return 14.0f + ((rpm - 2500.0f) * 0.5f / 1500.0f);
    else return 14.5f + fminf(((rpm - 4000.0f) * 0.5f / 2500.0f), 0.5f);
*/
      // Инвертируем диапазон 750-6500 в 800-1600
  return map((int)rpm, 750, 6500, (int)VOLTAGE_IDLE*100, 1600) / 100.0f;
}

float calculateFuelUsage(float rpm, unsigned long delta_ms) {
    if (rpm < RPM_IDLE || delta_ms == 0 || !settingsReceived) return 0.0f;
    
    float base_pulse_ms = INJECTOR_PULSE_MS;
    float pulse_correction = 1.0f;
    
    float estimated_speed = 0;
    if (rpm < 2000) estimated_speed = (rpm - RPM_IDLE) * 60.0f / (2000 - RPM_IDLE);
    else if (rpm < 2500) estimated_speed = 60.0f + (rpm - 2000) * 20.0f / 500.0f;
    else if (rpm < 3000) estimated_speed = 80.0f + (rpm - 2500) * 20.0f / 500.0f;
    else if (rpm < 3500) estimated_speed = 100.0f + (rpm - 3000) * 20.0f / 500.0f;
    else if (rpm < 4000) estimated_speed = 120.0f + (rpm - 3500) * 20.0f / 500.0f;
    else if (rpm < 5000) estimated_speed = 140.0f + (rpm - 4000) * 20.0f / 1000.0f;
    else estimated_speed = 160.0f + (rpm - 5000) * 20.0f / 800.0f;
    
    if (estimated_speed < 60.0f) {
        pulse_correction = 1.5f + (60.0f - estimated_speed) * 0.5f / 60.0f;
    } else if (estimated_speed >= 140.0f) {
        pulse_correction = 1.2f;
    } else {
        pulse_correction = 1.0f;
    }
    
    float adjusted_pulse_ms = base_pulse_ms * pulse_correction;
    
    float cycles_per_sec = (rpm / 60.0f) / 2.0f;
    float total_injector_open_time = cycles_per_sec * (adjusted_pulse_ms / 1000.0f) * deviceSettings.injector_count;
    float injector_perf_cm3_per_sec = deviceSettings.injector_performance / 60.0f;
    float fuel_cm3_per_sec = total_injector_open_time * injector_perf_cm3_per_sec;
    
    float high_rpm_correction = 1.0f;
    if (rpm > 4000.0f) {
        high_rpm_correction = 1.0f + ((rpm - 4000.0f) * 0.3f / 2500.0f);
    }
    
    return (fuel_cm3_per_sec * high_rpm_correction / 1000.0f) * (delta_ms / 1000.0f);
}

float calculateInstantConsumption(float speed, float fuel_usage_per_sec) {
    if (speed <= 0.1f || fuel_usage_per_sec <= 0) return 0.0f;
    float consumption = (fuel_usage_per_sec * 3600.0f / speed) * 100.0f;
    return fminf(fmaxf(consumption, 0.0f), 20.0f);
}

float calculateRemainingRange(float fuel_level, float consumption) {
    return (consumption > 0) ? (fuel_level / consumption) * 100.0f : 0.0f;
}

// === РАСЧЕТ ПЕРЕДАЧИ (4-СТУПЕНЧАТАЯ АКПП) ===
int calculateGear(float speed) {
    if (speed < 20.0f) return 1;      // 1-я: 0-20 км/ч
    else if (speed < 40.0f) return 2; // 2-я: 20-40 км/ч
    else if (speed < 60.0f) return 3; // 3-я: 40-60 км/ч
    else return 4;                    // 4-я: 60+ км/ч
}

// === УПРАВЛЕНИЕ СОСТОЯНИЕМ ПОДКЛЮЧЕНИЯ ===
void setConnectionState(bool connected) {
    static bool lastState = false;
    
    if (connected != lastState) {
        isBluetoothConnected = connected;
        lastState = connected;
        
        if (connected) {
            Serial.println("\n=== Bluetooth CONNECTED ===");
            digitalWrite(LED_BUILTIN, HIGH);
        } else {
            Serial.println("\n=== Bluetooth DISCONNECTED ===");
            stopAllCalculations();
            currentProgramState = STATE_WAITING_CONNECTION;
            settingsReceived = false;
            digitalWrite(LED_BUILTIN, LOW);
        }
    }
}

bool getConnectionState() {
    return isBluetoothConnected;
}

void stopAllCalculations() {
    isStreamingActive = false;
    forceSendData = false;
    
    if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        currentState.speed = 0.0f;
        currentState.rpm = RPM_IDLE;
        currentState.voltage = VOLTAGE_IDLE;
        currentState.fuel_consumption = 0.0f;
        currentState.remaining_range = 0.0f;
        currentState.gear = 1;
        xSemaphoreGive(dataMutex);
    }
    
    speedFilter.reset();
    rpmFilter.reset();
    voltageFilter.reset();
    
    lastSentSpeed = -1.0f;
    lastSentRPM = -1.0f;
    lastSentFuel = -1.0f;
    lastSendTime = 0;
    lastKeepAliveTime = 0;
    lastPhysicsCalcTime = 0;
    
    Serial.println("Calculations STOPPED");
}

void startAllCalculations() {
    isStreamingActive = true;
    Serial.println("Calculations STARTED");
    
    lastSentSpeed = -1.0f;
    lastSentRPM = -1.0f;
    lastSentFuel = -1.0f;
    lastSendTime = 0;
    lastKeepAliveTime = 0;
    lastPhysicsCalcTime = millis();
}

void updateFuelLevel() {
    if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        float new_fuel_level = deviceSettings.fuel_tank_capacity - currentState.fuel_used_total;
        
        if (new_fuel_level < 0) new_fuel_level = 0;
        if (new_fuel_level > deviceSettings.fuel_tank_capacity) {
            new_fuel_level = deviceSettings.fuel_tank_capacity;
        }
        
        currentState.fuel_level = roundf(new_fuel_level * 10.0f) / 10.0f;
        
        Serial.printf("Fuel updated: Tank=%.1fL, Used=%.1fL, Remaining=%.1fL\n",
                     deviceSettings.fuel_tank_capacity, 
                     currentState.fuel_used_total,
                     currentState.fuel_level);
        
        xSemaphoreGive(dataMutex);
    }
    forceSendData = true;
}

// === ЗАДАЧА 1: УПРАВЛЕНИЕ ПОДКЛЮЧЕНИЕМ ===
void connectionTask(void *parameter) {
    bool wasConnected = false;
    unsigned long lastConnectionTime = 0;
    
    while (1) {
        bool currentConnected = SerialBT.hasClient();
        unsigned long currentTime = millis();
        
        if (currentConnected && !wasConnected) {
            if (currentTime - lastConnectionTime < CONNECTION_DEBOUNCE_MS) {
                Serial.println("Ignoring rapid reconnection - debouncing");
                vTaskDelay(500 / portTICK_PERIOD_MS);
                continue;
            }
            lastConnectionTime = currentTime;
        }
        
        if (currentConnected != wasConnected) {
            setConnectionState(currentConnected);
            wasConnected = currentConnected;
        }
        
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
}

// === ЗАДАЧА 2: ФИЗИКА ===
void physicsTask(void *parameter) {
    unsigned long last_calc_time = millis();
    unsigned long last_adc_time = millis();
    
    float accumulated_trip_a = 0.0f;
    float accumulated_trip_b = 0.0f;
    float accumulated_odometer = 12450.7f;
    float filtered_fuel_per_sec = 0;
    
    analogReadResolution(12);
    analogSetAttenuation(ADC_11db);
    
    while (1) {
        if (!isStreamingActive) {
            vTaskDelay(100 / portTICK_PERIOD_MS);
            continue;
        }
        
        unsigned long current_time = millis();
        
        if (current_time - last_adc_time >= 20) {
            int raw_value = analogRead(POTENTIOMETER_PIN);
            float raw_speed = (raw_value / ADC_MAX) * SPEED_MAX;
            
            float filtered_speed = speedFilter.update(raw_speed);
            
            if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                // Скорость
                currentState.speed = roundf(filtered_speed * 10.0f) / 10.0f;
                
                // Передача (4-ступенчатая)
                currentState.gear = calculateGear(currentState.speed);
                
                // Обороты - ИСПРАВЛЕНО!
                float rawRPM = calculateRPM(currentState.speed);
                currentState.rpm = roundf(rpmFilter.update(rawRPM));
                
                // Напряжение
                float rawVoltage = calculateVoltage(currentState.rpm);
                currentState.voltage = roundf(rawVoltage* 100.0f) / 100.0f;
                
                // Время
                unsigned long delta_ms = (last_calc_time > 0) ? 
                    (current_time - last_calc_time) : 50;
                
                // Пробег
                float distance_km = currentState.speed * (delta_ms / 3600000.0f);
                accumulated_trip_a += distance_km;
                accumulated_trip_b += distance_km;
                accumulated_odometer += distance_km;
                
                currentState.trip_a = roundf(accumulated_trip_a * 100.0f) / 100.0f;
                currentState.trip_b = roundf(accumulated_trip_b * 100.0f) / 100.0f;
                currentState.odometer = roundf(accumulated_odometer * 100.0f) / 100.0f;
                
                // Топливо
                if (settingsReceived) {
                    float fuel_used = calculateFuelUsage(currentState.rpm, delta_ms);
                    currentState.fuel_used_total += fuel_used;
                    
                    float new_fuel_level = deviceSettings.fuel_tank_capacity - currentState.fuel_used_total;
                    if (new_fuel_level < 0) new_fuel_level = 0;
                    
                    currentState.fuel_level = currentState.fuel_level * 0.9f + new_fuel_level * 0.1f;
                    currentState.fuel_level = roundf(currentState.fuel_level * 10.0f) / 10.0f;
                    
                    float raw_fuel_per_sec = (delta_ms > 0) ? fuel_used / (delta_ms / 1000.0f) : 0;
                    filtered_fuel_per_sec = filtered_fuel_per_sec * 0.8f + raw_fuel_per_sec * 0.2f;
                    
                    currentState.fuel_consumption = calculateInstantConsumption(
                        currentState.speed, filtered_fuel_per_sec);
                    currentState.fuel_consumption = roundf(currentState.fuel_consumption * 10.0f) / 10.0f;
                    
                    currentState.remaining_range = calculateRemainingRange(
                        currentState.fuel_level, currentState.fuel_consumption);
                    currentState.remaining_range = roundf(currentState.remaining_range * 10.0f) / 10.0f;
                }
                
                // Отладка каждые 100 мс
                static int debug_counter = 0;
                debug_counter++;
                if (debug_counter >= 5) {
                    debug_counter = 0;
                    Serial.printf("Physics: Speed=%.1f km/h, RPM=%.0f, Gear=%d, RawADC=%d, RawSpeed=%.1f\n", 
                                 currentState.speed, 
                                 currentState.rpm,
                                 currentState.gear,
                                 raw_value,
                                 raw_speed);
                }
                
                xSemaphoreGive(dataMutex);
                last_calc_time = current_time;
            }
            
            last_adc_time = current_time;
        }
        
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}

// === ФУНКЦИИ BLUETOOTH ===
void sendJSONResponse(const String& key, const String& value) {
    if (!getConnectionState()) return;
    
    JsonDocument doc;
    doc[key] = value;
    
    String jsonString;
    serializeJson(doc, jsonString);
    SerialBT.println(jsonString);
    Serial.println("Sent: " + jsonString);
}

void sendCarData() {
    if (!getConnectionState()) return;
    
    unsigned long currentTime = millis();
    
    JsonDocument doc;
    CarState snapshot;
    
    if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        snapshot = currentState;
        xSemaphoreGive(dataMutex);
    } else {
        return;
    }
    
    bool timeForSend = (currentTime - lastSendTime) >= MIN_SEND_INTERVAL_MS;
    if (!timeForSend && !forceSendData) return;
    
    if (forceSendData) {
        forceSendData = false;
    } else {
        bool speedChanged = fabs(snapshot.speed - lastSentSpeed) >= SPEED_CHANGE_THRESHOLD;
        bool rpmChanged = fabs(snapshot.rpm - lastSentRPM) >= RPM_CHANGE_THRESHOLD;
        bool fuelChanged = fabs(snapshot.fuel_level - lastSentFuel) >= FUEL_CHANGE_THRESHOLD;
        
        if (!speedChanged && !rpmChanged && !fuelChanged) {
            if (currentTime - lastKeepAliveTime < 500) return;
            lastKeepAliveTime = currentTime;
        }
    }
    
    lastSentSpeed = snapshot.speed;
    lastSentRPM = snapshot.rpm;
    lastSentFuel = snapshot.fuel_level;
    lastSendTime = currentTime;
    
    JsonObject data = doc["data"].to<JsonObject>();
    data["speed"] = snapshot.speed;
    data["voltage"] = snapshot.voltage;
    data["trip_a"] = snapshot.trip_a;
    data["trip_b"] = snapshot.trip_b;
    data["odometer"] = snapshot.odometer;
    data["fuel"] = snapshot.fuel_level;
    data["fuel_consumption"] = snapshot.fuel_consumption;
    data["remaining_range"] = snapshot.remaining_range;
    data["rpm"] = snapshot.rpm;
    data["gear"] = snapshot.gear;
    
    JsonObject settings_info = doc["settings_info"].to<JsonObject>();
    settings_info["tank_capacity"] = deviceSettings.fuel_tank_capacity;
    settings_info["injector_count"] = deviceSettings.injector_count;
    settings_info["injector_performance"] = deviceSettings.injector_performance;
    settings_info["fuel_used_total"] = snapshot.fuel_used_total;
    
    String jsonString;
    serializeJson(doc, jsonString);
    SerialBT.println(jsonString);
    
    // Вывод в терминал со скоростью, оборотами И ПЕРЕДАЧЕЙ
    Serial.printf("Data sent: Speed=%.1f km/h, RPM=%.0f, Gear=%d, Tank=%.1fL, Used=%.1fL, Remaining=%.1fL\n", 
                 snapshot.speed, 
                 snapshot.rpm,
                 snapshot.gear,
                 deviceSettings.fuel_tank_capacity,
                 snapshot.fuel_used_total,
                 snapshot.fuel_level);
}

// === БЕЗОПАСНАЯ ОБРАБОТКА JSON ===
void processIncomingJSON(const String& jsonString) {
    Serial.println("Received: " + jsonString);
    
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, jsonString);
    if (error) {
        Serial.println("JSON error: " + String(error.c_str()));
        return;
    }
    
    if (doc["settings"].is<JsonObject>()) {
        JsonObject settings = doc["settings"];
        
        bool settingsUpdated = false;
        float old_fuel_tank = deviceSettings.fuel_tank_capacity;
        float old_injector_perf = deviceSettings.injector_performance;
        int old_injector_count = deviceSettings.injector_count;
        float old_odometer = currentState.odometer;
        
        if (settings["fuel_tank_capacity"].is<float>()) {
            deviceSettings.fuel_tank_capacity = settings["fuel_tank_capacity"];
            settingsUpdated = true;
        }
        
        if (settings["injector_count"].is<int>()) {
            deviceSettings.injector_count = settings["injector_count"];
            settingsUpdated = true;
        }
        
        if (settings["injector_performance"].is<float>()) {
            deviceSettings.injector_performance = settings["injector_performance"];
            settingsUpdated = true;
        }
        
        if (settings["speed_sensor_signals"].is<int>()) {
            deviceSettings.speed_sensor_signals = settings["speed_sensor_signals"];
        }
        
        if (settings["initial_odometer"].is<float>()) {
            float new_odometer = settings["initial_odometer"].as<float>();
            if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                currentState.odometer = new_odometer;
                xSemaphoreGive(dataMutex);
            }
            settingsUpdated = true;
            Serial.printf("  Odometer: %.1f km -> %.1f km\n", old_odometer, new_odometer);
        }
        
        if (settings["initial_fuel"].is<float>()) {
            float initial_fuel = settings["initial_fuel"].as<float>();
            deviceSettings.initial_fuel = initial_fuel;
            
            if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                currentState.fuel_level = initial_fuel;
                currentState.fuel_used_total = deviceSettings.fuel_tank_capacity - initial_fuel;
                if (currentState.fuel_used_total < 0) currentState.fuel_used_total = 0;
                xSemaphoreGive(dataMutex);
            }
            settingsUpdated = true;
            Serial.printf("  Initial fuel: %.1f L\n", initial_fuel);
        }
        
        if (settingsUpdated) {
            settingsReceived = true;
            Serial.println("Settings updated:");
            if (old_fuel_tank != deviceSettings.fuel_tank_capacity)
                Serial.printf("  Tank: %.1fL -> %.1fL\n", old_fuel_tank, deviceSettings.fuel_tank_capacity);
            if (old_injector_count != deviceSettings.injector_count)
                Serial.printf("  Injectors: %d -> %d\n", old_injector_count, deviceSettings.injector_count);
            if (old_injector_perf != deviceSettings.injector_performance)
                Serial.printf("  Performance: %.0f -> %.0f cc/min\n", old_injector_perf, deviceSettings.injector_performance);
            
            updateFuelLevel();
        }
        
        sendJSONResponse("settings", "OK");
        
        if (currentProgramState != STATE_STREAMING_DATA)
            currentProgramState = STATE_IDLE;
        return;
    }
    
    if (doc["command"].is<String>()) {
        String command = doc["command"].as<String>();
        
        if (command == "GET_DATA") {
            sendJSONResponse("GET_DATA", "OK");
            currentProgramState = STATE_STREAMING_DATA;
            startAllCalculations();
            return;
        }
        
        if (command == "STOP") {
            stopAllCalculations();
            currentProgramState = STATE_IDLE;
            sendJSONResponse("stop", "OK");
            return;
        }
        
        if (command == "RESET_FUEL") {
            if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                currentState.fuel_used_total = 0;
                xSemaphoreGive(dataMutex);
            }
            sendJSONResponse("reset_fuel", "OK");
            forceSendData = true;
            return;
        }
        
        if (command == "RESET_TRIP_A") {
            if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                currentState.trip_a = 0;
                xSemaphoreGive(dataMutex);
            }
            sendJSONResponse("reset_trip_a", "OK");
            forceSendData = true;
            return;
        }
        
        if (command == "RESET_TRIP_B") {
            if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                currentState.trip_b = 0;
                xSemaphoreGive(dataMutex);
            }
            sendJSONResponse("reset_trip_b", "OK");
            forceSendData = true;
            return;
        }
        
        if (command == "RESET_ALL_TRIPS") {
            if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                currentState.trip_a = 0;
                currentState.trip_b = 0;
                xSemaphoreGive(dataMutex);
            }
            sendJSONResponse("reset_all_trips", "OK");
            forceSendData = true;
            return;
        }
        
        sendJSONResponse("error", "Unknown command: " + command);
        return;
    }
    
    sendJSONResponse("error", "Invalid JSON format");
}

// === ЗАДАЧА 3: BLUETOOTH ===
void bluetoothTask(void *parameter) {
    while (1) {
        if (getConnectionState()) {
            if (SerialBT.available()) {
                String incoming = SerialBT.readStringUntil('\n');
                incoming.trim();
                if (incoming.length() > 0) {
                    processIncomingJSON(incoming);
                }
            }
            
            if (currentProgramState == STATE_STREAMING_DATA && isStreamingActive) {
                sendCarData();
                digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
            }
        }
        
        vTaskDelay(20 / portTICK_PERIOD_MS);
    }
}

// === SETUP ===
void setup() {
    Serial.begin(115200);
    delay(2000);
    
    Serial.println("\n\n=== Car Physics Simulator ESP32 ===");
    Serial.println("4-Speed Automatic Transmission");
    Serial.println("Adaptive Filter + Safe JSON");
    Serial.println("FIXED: RPM curve 0-60: 12.5, 60+: 25");
    Serial.println("=============================================\n");
    
    dataMutex = xSemaphoreCreateMutex();
    if (dataMutex == NULL) {
        Serial.println("ERROR: Failed to create mutex!");
        while(1);
    }
    
    SerialBT.begin("Car Simulator");
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, LOW);
    
    isBluetoothConnected = false;
    isStreamingActive = false;
    currentProgramState = STATE_WAITING_CONNECTION;
    settingsReceived = false;
    
    Serial.println("System initialized.");
    Serial.println("\n=== 4-SPEED TRANSMISSION ===");
    Serial.println("  1st gear:   0-20 km/h");
    Serial.println("  2nd gear:  20-40 km/h");
    Serial.println("  3rd gear:  40-60 km/h");
    Serial.println("  4th gear:   60+ km/h");
    
    Serial.println("\n=== RPM CURVE (FIXED) ===");
    Serial.println("   0 km/h →  750 RPM");
    Serial.println("  60 km/h → 1500 RPM");
    Serial.println("  80 km/h → 2000 RPM");
    Serial.println(" 100 km/h → 2500 RPM");
    Serial.println(" 120 km/h → 3000 RPM");
    Serial.println(" 140 km/h → 3500 RPM");
    Serial.println(" 160 km/h → 4000 RPM");
    Serial.println(" 180 km/h → 4500 RPM");
    Serial.println(" 200 km/h → 5000 RPM");
    
    Serial.println("\n=== ADAPTIVE FILTER ===");
    Serial.println("  Noise level: 0.2 km/h");
    Serial.println("  Fast threshold: 5.0 km/h");
    Serial.println("  Alpha slow: 0.05");
    Serial.println("  Alpha fast: 0.7");
    
    Serial.println("\n=== COMMANDS ===");
    Serial.println("  {\"settings\":{\"fuel_tank_capacity\":60.0, \"initial_odometer\":15234.5}}");
    Serial.println("  {\"command\":\"GET_DATA\"} - Start");
    Serial.println("  {\"command\":\"STOP\"} - Stop");
    Serial.println("  {\"command\":\"RESET_FUEL\"} - Full tank");
    Serial.println("  {\"command\":\"RESET_TRIP_A\"} - Reset trip A");
    Serial.println("  {\"command\":\"RESET_TRIP_B\"} - Reset trip B");
    Serial.println("  {\"command\":\"RESET_ALL_TRIPS\"} - Reset all trips");
    Serial.println("=============================================\n");
    
    xTaskCreatePinnedToCore(connectionTask, "Connection", TASK_STACK_SIZE, NULL, 3, &connectionTaskHandle, 0);
    xTaskCreatePinnedToCore(physicsTask, "Physics", TASK_STACK_SIZE, NULL, 2, &physicsTaskHandle, 0);
    xTaskCreatePinnedToCore(bluetoothTask, "Bluetooth", TASK_STACK_SIZE, NULL, 1, &bluetoothTaskHandle, 1);
    
    Serial.println("Tasks created. Ready for connection.\n");
}

void loop() {
    vTaskDelay(1000 / portTICK_PERIOD_MS);
}