#pragma once

// -----------------------------------------------------------------------------
// protocol_json.h
// JSON-протокол обмена с Android:
// - чтение входящих сообщений
// - отправка телеметрии
// - отправка подтверждений (ack_id) по msg_id
// -----------------------------------------------------------------------------

#include <Arduino.h>

// sendJSONResponse:
// Отправляет простое JSON-сообщение вида { "<key>": "<value>" } по Bluetooth.
void sendJSONResponse(const String& key, const String& value);

// sendMessageAck:
// Отправляет квитанцию формата {"ack_id":"<msgId>"} для гарантии доставки.
void sendMessageAck(const String& msgId);

// sendCarData:
// Формирует и отправляет телеметрию (данные авто + settings_info).
void sendCarData();

// processIncomingJSON:
// Парсит входящую строку JSON, обновляет settings/состояние и выполняет команды.
void processIncomingJSON(const String& jsonString);
