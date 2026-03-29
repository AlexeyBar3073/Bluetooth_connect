#pragma once

// -----------------------------------------------------------------------------
// filters.h
// Фильтры сигналов для сглаживания телеметрии.
// Эти фильтры не зависят от Bluetooth/FreeRTOS и хорошо переносятся на будущую
// замену источника данных (симулятор -> железо).
// -----------------------------------------------------------------------------

#include <math.h>

struct AdaptiveFilter {
    // Текущее сглаженное значение
    float value = 0;

    // Флаг: инициализирован ли фильтр первым значением
    bool initialized = false;

    // update: обновляет сглаженное значение по входному сигналу.
    // Алгоритм адаптируется к "размеру шага" (diff):
    // - если diff небольшой: alpha мала (сглаживание)
    // - если diff большой: alpha больше (реакция быстрее)
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
        } else if (diff > fastThreshold) {
            alpha = alphaFast;
        } else {
            float t = (diff - noiseLevel) / (fastThreshold - noiseLevel);
            alpha = alphaSlow + t * (alphaFast - alphaSlow);
        }

        value = value + alpha * (input - value);
        return value;
    }

    // reset: сбрасывает состояние фильтра (следующее update снова примет вход как базу)
    void reset() {
        initialized = false;
        value = 0;
    }
};

struct SimpleFilter {
    // Текущее сглаженное значение (как простой экспоненциальный фильтр)
    float lastValue = 0;

    // Флаг: инициализирован ли фильтр первым значением
    bool initialized = false;

    // Коэффициент сглаживания (0..1). Чем больше alpha, тем быстрее реакция.
    const float alpha = 0.3f;

    // update: экспоненциальное сглаживание.
    // new = old*(1-alpha) + input*alpha
    float update(float input) {
        if (!initialized) {
            lastValue = input;
            initialized = true;
            return input;
        }

        lastValue = lastValue * (1 - alpha) + input * alpha;
        return lastValue;
    }

    // reset: сбрасывает состояние фильтра
    void reset() {
        initialized = false;
        lastValue = 0;
    }
};
