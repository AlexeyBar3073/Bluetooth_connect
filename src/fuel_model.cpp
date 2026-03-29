#include <math.h>
#include "fuel_model.h"
#include "app_config.h"

// -----------------------------------------------------------------------------
// fuel_model.cpp
// Расчётная модель топлива:
// - оценивает использованное топливо по RPM и времени
// - оценивает мгновенный расход и остаточный запас хода
// -----------------------------------------------------------------------------

// calculateFuelUsage:
// Возвращает оценку израсходованного топлива (в литрах) за delta_ms.
// settings_ok нужен, чтобы не начинать расчёт топлива до получения калибровок.
float calculateFuelUsage(float rpm, unsigned long delta_ms,
                         const Settings& deviceSettings, bool settings_ok) {
    // Если не заведены настройки или RPM слишком мал / времени не было — расход = 0
    if (rpm < RPM_IDLE || delta_ms == 0 || !settings_ok) return 0.0f;

    // base_pulse_ms: длина базового "импульса" форсунки (мс) в модели
    float base_pulse_ms = INJECTOR_PULSE_MS;
    // pulse_correction: множитель поправки импульса (масштабирует расход в зависимости от оценочной скорости)
    float pulse_correction = 1.0f;

    // estimated_speed: вспомогательная оценка скорости по RPM (чтобы сделать коррекцию на режим)
    float estimated_speed = 0;
    if (rpm < 2000) estimated_speed = (rpm - RPM_IDLE) * 60.0f / (2000 - RPM_IDLE);
    else if (rpm < 2500) estimated_speed = 60.0f + (rpm - 2000) * 20.0f / 500.0f;
    else if (rpm < 3000) estimated_speed = 80.0f + (rpm - 2500) * 20.0f / 500.0f;
    else if (rpm < 3500) estimated_speed = 100.0f + (rpm - 3000) * 20.0f / 500.0f;
    else if (rpm < 4000) estimated_speed = 120.0f + (rpm - 3500) * 20.0f / 500.0f;
    else if (rpm < 5000) estimated_speed = 140.0f + (rpm - 4000) * 20.0f / 1000.0f;
    else estimated_speed = 160.0f + (rpm - 5000) * 20.0f / 800.0f;

    // Коррекция по режиму: на низкой и высокой скорости расход увеличивается.
    if (estimated_speed < 60.0f) {
        pulse_correction = 1.5f + (60.0f - estimated_speed) * 0.5f / 60.0f;
    } else if (estimated_speed >= 140.0f) {
        pulse_correction = 1.2f;
    } else {
        pulse_correction = 1.0f;
    }

    // adjusted_pulse_ms: итоговая длительность "импульса" после поправки.
    float adjusted_pulse_ms = base_pulse_ms * pulse_correction;

    // cycles_per_sec: число "циклов" впрыска в секунду (приблизительно, через обороты).
    float cycles_per_sec = (rpm / 60.0f) / 2.0f;
    // total_injector_open_time: суммарное время открытия форсунок в интервале.
    float total_injector_open_time =
        cycles_per_sec * (adjusted_pulse_ms / 1000.0f) * deviceSettings.injector_count;
    // injector_perf_cm3_per_sec: производительность одной форсунки в см^3/с.
    float injector_perf_cm3_per_sec = deviceSettings.injector_performance / 60.0f;
    // fuel_cm3_per_sec: объём топлива в см^3/с (умножаем открытое время на производительность).
    float fuel_cm3_per_sec = total_injector_open_time * injector_perf_cm3_per_sec;

    // high_rpm_correction: дополнительная поправка на высокие обороты.
    float high_rpm_correction = 1.0f;
    if (rpm > 4000.0f) {
        high_rpm_correction = 1.0f + ((rpm - 4000.0f) * 0.3f / 2500.0f);
    }

    // Возвращаем топливо в литрах за delta_ms.
    return (fuel_cm3_per_sec * high_rpm_correction / 1000.0f) * (delta_ms / 1000.0f);
}

// calculateInstantConsumption:
// Пересчёт объёма топлива за секунду в л/100км.
float calculateInstantConsumption(float speed, float fuel_usage_per_sec) {
    // speed <= 0 или нулевой расход -> нулевой расход л/100км.
    if (speed <= 0.1f || fuel_usage_per_sec <= 0) return 0.0f;
    float consumption = (fuel_usage_per_sec * 3600.0f / speed) * 100.0f;
    // Ограничиваем диапазон, чтобы из-за шумов не получать экстремальные числа.
    return fminf(fmaxf(consumption, 0.0f), 20.0f);
}

// calculateRemainingRange:
// Запас хода в процентах-условной метрике: fuel_level / consumption * 100.
float calculateRemainingRange(float fuel_level, float consumption) {
    return (consumption > 0) ? (fuel_level / consumption) * 100.0f : 0.0f;
}
