#pragma once

// -----------------------------------------------------------------------------
// fuel_model.h
// Доменная логика модели топлива: пересчёт из оборотов/времени в использованное
// топливо, вычисление мгновенного расхода и запаса хода.
// -----------------------------------------------------------------------------

#include "app_types.h"

// calculateFuelUsage:
// Оценка использованного топлива (л) за интервал delta_ms, используя модель
// форсунок (injector_count, injector_performance) и косвенную оценку скорости.
float calculateFuelUsage(float rpm, unsigned long delta_ms,
                         const Settings& settings, bool settings_ok);

// calculateInstantConsumption:
// Вычисляет мгновенный расход в л/100км по текущей скорости и топливу за секунду.
float calculateInstantConsumption(float speed, float fuel_usage_per_sec);

// calculateRemainingRange:
// Вычисляет запас хода (условно, км) по текущему уровню топлива и расходу.
float calculateRemainingRange(float fuel_level, float consumption);
