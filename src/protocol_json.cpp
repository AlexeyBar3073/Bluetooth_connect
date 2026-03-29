#include <Arduino.h>
#include <math.h>
#include <ArduinoJson.h>
#include "protocol_json.h"
#include "app_state.h"
#include "app_config.h"
#include "runtime.h"

// -----------------------------------------------------------------------------
// protocol_json.cpp
// Реализация JSON-протокола для обмена с Android:
// - Парсинг входящих сообщений (settings / command)
// - Отправка телеметрии (data + settings_info)
// - Отправка ack по msg_id для обеспечения ретраев на стороне Android
// -----------------------------------------------------------------------------

// sendJSONResponse:
// Отправляет в BT строку JSON вида { "<key>": "<value>" }.
// Этот хелпер используется для коротких подтверждений команд.
void sendJSONResponse(const String& key, const String& value) {
    if (!getConnectionState()) return;

    // doc: временный JSON-буфер (ArduinoJson) для сериализации ответа.
    JsonDocument doc;
    // <key>: значение <value>
    doc[key] = value;

    // jsonString: итоговая строка, которую отправляем в Bluetooth.
    String jsonString;
    serializeJson(doc, jsonString);
    SerialBT.println(jsonString);
    Serial.println("Sent: " + jsonString);
}

// variantMsgIdToString:
// Android может прислать msg_id как число или строку.
// Эта функция приводит JsonVariant к строковому представлению msg_id,
// чтобы ack_id совпадал с тем, что Android считает идентификатором.
static String variantMsgIdToString(JsonVariant v) {
    if (v.isNull()) return String();
    if (v.is<const char*>()) return String(v.as<const char*>());
    if (v.is<int>()) return String(v.as<int>());
    if (v.is<long>()) return String(v.as<long>());
    if (v.is<unsigned long>()) return String(v.as<unsigned long>());
    if (v.is<double>()) {
        // double d: преобразование с учётом, что id могли передать как float.
        double d = v.as<double>();
        if (d == floor(d)) return String((long long)d);
        return String(d, 10);
    }
    if (v.is<bool>()) return v.as<bool>() ? String("true") : String("false");
    return String();
}

// sendMessageAck:
// Унифицированная квитанция доставки для протокола Android.
// Формат: {"ack_id":"<msgId>"}.
void sendMessageAck(const String& msgId) {
    if (!getConnectionState() || msgId.length() == 0) return;

    // doc: JSON-буфер под ack.
    JsonDocument doc;
    // ack_id: msg_id из входящего сообщения
    doc["ack_id"] = msgId;

    // Сериализуем и отправляем
    String jsonString;
    serializeJson(doc, jsonString);
    SerialBT.println(jsonString);
    Serial.println("Sent: " + jsonString);
}

// sendCarData:
// Отправляет большой пакет телеметрии в формате:
// { "data": {...}, "settings_info": {...} }
// Отправка оптимизируется порогами изменений и минимальным интервалом.
void sendCarData() {
    if (!getConnectionState()) return;

    // currentTime: текущее время (millis) для интервалов отправки.
    unsigned long currentTime = millis();

    // doc: буфер для полного JSON-сообщения.
    JsonDocument doc;
    // snapshot: копия currentState под mutex, чтобы сформировать пакет консистентно.
    CarState snapshot;

    if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        snapshot = currentState;
        xSemaphoreGive(dataMutex);
    } else {
        return;
    }

    // timeForSend: прошло ли достаточно времени с момента последней отправки.
    bool timeForSend = (currentTime - lastSendTime) >= MIN_SEND_INTERVAL_MS;
    if (!timeForSend && !forceSendData) return;

    // forceSendData: если принудительный флаг установлен, отправляем независимо от порогов.
    if (forceSendData) {
        forceSendData = false;
    } else {
        // speedChanged / rpmChanged / fuelChanged: пороговые проверки для "экономной" отправки.
        bool speedChanged = fabs(snapshot.speed - lastSentSpeed) >= SPEED_CHANGE_THRESHOLD;
        bool rpmChanged = fabs(snapshot.rpm - lastSentRPM) >= RPM_CHANGE_THRESHOLD;
        bool fuelChanged = fabs(snapshot.fuel_level - lastSentFuel) >= FUEL_CHANGE_THRESHOLD;

        if (!speedChanged && !rpmChanged && !fuelChanged) {
            if (currentTime - lastKeepAliveTime < 500) return;
            lastKeepAliveTime = currentTime;
        }
    }

    // Обновляем историю отправок.
    lastSentSpeed = snapshot.speed;
    lastSentRPM = snapshot.rpm;
    lastSentFuel = snapshot.fuel_level;
    lastSendTime = currentTime;

    // data: основной блок телеметрии.
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
    data["ecu_errors"] = "P0141";

    // settings_info: некоторые настройки и сервисные величины, которые полезны приложению.
    JsonObject settings_info = doc["settings_info"].to<JsonObject>();
    settings_info["tank_capacity"] = deviceSettings.fuel_tank_capacity;
    settings_info["injector_count"] = deviceSettings.injector_count;
    settings_info["injector_performance"] = deviceSettings.injector_performance;
    settings_info["fuel_used_total"] = snapshot.fuel_used_total;

    String jsonString;
    serializeJson(doc, jsonString);
    SerialBT.println(jsonString);

    // Лог в терминал, чтобы было видно отправленный пакет.
    Serial.printf("Data sent: Speed=%.1f km/h, RPM=%.0f, Gear=%d, Tank=%.1fL, Used=%.1fL, Remaining=%.1fL\n",
                  snapshot.speed,
                  snapshot.rpm,
                  snapshot.gear,
                  deviceSettings.fuel_tank_capacity,
                  snapshot.fuel_used_total,
                  snapshot.fuel_level);
}

// processIncomingJSON:
// Обрабатывает входящую строку JSON от Android:
// - сначала парсит msg_id и отправляет ack (если msg_id присутствует)
// - затем обрабатывает settings или command
void processIncomingJSON(const String& jsonString) {
    Serial.println("Received: " + jsonString);

    // doc: временный буфер для парсинга.
    JsonDocument doc;
    // error: результат deserializeJson
    DeserializationError error = deserializeJson(doc, jsonString);
    if (error) {
        Serial.println("JSON error: " + String(error.c_str()));
        return;
    }

    // msgId: входящий идентификатор сообщения (может быть числом/строкой).
    String msgId = variantMsgIdToString(doc["msg_id"]);
    if (msgId.length() > 0) {
        sendMessageAck(msgId);
    }

    if (doc["settings"].is<JsonObject>()) {
        // settings: объект с параметрами калибровки/инициализации.
        JsonObject settings = doc["settings"];

        // settingsUpdated: нужно ли было что-то изменить (для установки settingsReceived и пересчёта топлива).
        bool settingsUpdated = false;

        // old_*: для логирования разницы до/после.
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
                Serial.printf("  Performance: %.0f -> %.0f cc/min\n", old_injector_perf,
                              deviceSettings.injector_performance);

            updateFuelLevel();
        }

        sendJSONResponse("settings", "OK");

        if (currentProgramState != STATE_STREAMING_DATA)
            currentProgramState = STATE_IDLE;
        return;
    }

    if (doc["command"].is<String>()) {
        // command: строковая команда
        String command = doc["command"].as<String>();

        if (command == "GET_DATA") {
            sendJSONResponse("GET_DATA", "OK");
            // Переходим в режим стриминга и запускаем вычисления
            currentProgramState = STATE_STREAMING_DATA;
            startAllCalculations();
            return;
        }

        if (command == "STOP") {
            // STOP: полностью останавливаем расчёты/стриминг
            stopAllCalculations();
            currentProgramState = STATE_IDLE;
            sendJSONResponse("stop", "OK");
            return;
        }

        if (command == "RESET_FUEL") {
            // RESET_FUEL: обнуляем суммарно использованное топливо,
            // чтобы fuel_level пересчитался через updateFuelLevel().
            if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                currentState.fuel_used_total = 0;
                xSemaphoreGive(dataMutex);
            }
            sendJSONResponse("reset_fuel", "OK");
            // forceSendData: надо дать Android увидеть изменение топлива сразу.
            forceSendData = true;
            return;
        }

        if (command == "RESET_TRIP_A") {
            // RESET_TRIP_A: обнуляем счётчик пробега поездки A
            if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                currentState.trip_a = 0;
                xSemaphoreGive(dataMutex);
            }
            sendJSONResponse("reset_trip_a", "OK");
            forceSendData = true;
            return;
        }

        if (command == "RESET_TRIP_B") {
            // RESET_TRIP_B: обнуляем счётчик пробега поездки B
            if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                currentState.trip_b = 0;
                xSemaphoreGive(dataMutex);
            }
            sendJSONResponse("reset_trip_b", "OK");
            forceSendData = true;
            return;
        }

        if (command == "RESET_ALL_TRIPS") {
            // RESET_ALL_TRIPS: обнуляем оба "trip"
            if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                currentState.trip_a = 0;
                currentState.trip_b = 0;
                xSemaphoreGive(dataMutex);
            }
            sendJSONResponse("reset_all_trips", "OK");
            forceSendData = true;
            return;
        }

        // Любая неизвестная команда
        sendJSONResponse("error", "Unknown command: " + command);
        return;
    }

    // Если JSON валиден, но не содержит settings и не содержит command — это ошибка формата.
    sendJSONResponse("error", "Invalid JSON format");
}
