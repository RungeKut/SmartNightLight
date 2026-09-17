/******************************************************************
 * NightLight.h — вся логика светильника: что должно гореть и почему
 *
 * Модуль намеренно не знает ни про Arduino, ни про WiFi, ни про
 * EEPROM: на вход — настройки и текущее время, на выход — яркость и
 * режим. Поэтому он собирается хостовым компилятором и покрыт
 * тестами (pio test -e native).
 ******************************************************************/

#ifndef NightLight_h
#define NightLight_h

#include <stdint.h>

namespace nl {

// ==================== Формат времени ====================
//
// Время суток хранится как hhmm: часы * 100 + минуты (22:30 -> 2230).
// Формат достался от первой версии, где сутки паковались в один
// uint32_t, и сохранён ради совместимости с настройками в EEPROM.
// Внутри все расчёты идут в минутах от полуночи.

const uint16_t MINUTES_PER_DAY = 24 * 60;

inline uint16_t hhmmToMinutes(uint16_t hhmm) {
  uint16_t h = hhmm / 100;
  uint16_t m = hhmm % 100;
  if (h > 23) h = 23;
  if (m > 59) m = 59;
  return h * 60 + m;
}

inline uint16_t minutesToHhmm(uint16_t minutes) {
  minutes %= MINUTES_PER_DAY;
  return (minutes / 60) * 100 + (minutes % 60);
}

// Сколько минут прошло от start до now. Результат всегда 0..1439,
// поэтому переход через полночь обрабатывается сам собой: интервал
// "внутри" — это просто elapsed <= длительность.
inline uint16_t minutesSince(uint16_t startHhmm, uint16_t nowHhmm) {
  int32_t start = hhmmToMinutes(startHhmm);
  int32_t now = hhmmToMinutes(nowHhmm);
  int32_t diff = now - start;
  if (diff < 0) diff += MINUTES_PER_DAY;
  return (uint16_t)diff;
}

// ==================== Настройки ====================

// Биты маски включённых режимов. Значения совпадают с первой
// версией прошивки — та же маска лежит в EEPROM.
const uint8_t FLAG_SLEEP   = 0x01;  // режим отхода ко сну
const uint8_t FLAG_SIGNAL  = 0x02;  // вспышки "пора спать"
const uint8_t FLAG_DIM     = 0x04;  // затухание при засыпании
const uint8_t FLAG_SUNRISE = 0x08;  // имитация рассвета

// Длительность вспышек "пора спать", минут от начала интервала сна
const uint8_t SIGNAL_MINUTES = 5;

// Ниже этого значения светодиод уже не тлеет, а мерцает
const float MIN_BRIGHTNESS = 3.0f;

struct Settings {
  uint8_t  flags;              // маска FLAG_*
  uint8_t  manualBrightness;   // яркость ручного включения
  uint16_t sleepTime;          // hhmm, начало отхода ко сну
  uint8_t  sleepLength;        // минут на засыпание
  uint8_t  signalBrightness;   // яркость вспышек "пора спать"
  uint16_t sunriseTime;        // hhmm, начало рассвета
  uint8_t  sunriseLength;      // минут на разгорание
  uint8_t  sunLength;          // минут держать максимум после рассвета
  uint8_t  sunriseBrightness;  // максимум рассвета
};

// ==================== Режимы ====================

enum Mode : uint8_t {
  MODE_OFF = 0,   // погашен
  MODE_MANUAL,    // включён вручную с вкладки Dashboard
  MODE_SIGNAL,    // вспышки "пора спать"
  MODE_DIM,       // затухание при засыпании
  MODE_SLEEP,     // интервал сна, затухание выключено
  MODE_SUNRISE,   // имитация рассвета
};

inline const char* modeName(Mode m) {
  switch (m) {
    case MODE_MANUAL:  return "manual";
    case MODE_SIGNAL:  return "signal";
    case MODE_DIM:     return "dim";
    case MODE_SLEEP:   return "sleep";
    case MODE_SUNRISE: return "sunrise";
    default:           return "off";
  }
}

struct Result {
  uint8_t brightness;
  Mode    mode;
};

// ==================== Кривые яркости ====================
//
// Обе кривые — одна и та же парабола с вершиной на краю интервала,
// записанная в явном виде:
//
//   затухание: y(x) = min + (max-min) * ((L-x)/L)^2   — max в начале
//   рассвет:   y(x) = min + (max-min) * (x/L)^2       — max в конце
//
// Раньше коэффициенты a, b, c считались отдельно. Для затухания это
// была та же формула, а вот у рассвета знаменатель (L - L^2/2)
// обращался в ноль при длительности ровно 2 минуты, и яркость
// становилась NaN. При обычных 60 минутах кривые совпадают с
// точностью до единицы яркости.

inline uint8_t clampBrightness(float v) {
  if (v < 0.0f) return 0;
  if (v > 255.0f) return 255;
  return (uint8_t)(v + 0.5f);
}

// Доля пути, возведённая в квадрат. Вынесена, чтобы обе кривые
// одинаково вели себя при нулевой длительности интервала.
inline float squaredRatio(uint16_t position, uint16_t length) {
  if (length == 0) return 1.0f;   // интервала нет — сразу конечная точка
  if (position >= length) return 1.0f;
  float r = (float)position / (float)length;
  return r * r;
}

inline uint8_t curveBrightness(float maxBrightness, float ratioSquared) {
  if (maxBrightness < MIN_BRIGHTNESS) maxBrightness = MIN_BRIGHTNESS;
  return clampBrightness(MIN_BRIGHTNESS + (maxBrightness - MIN_BRIGHTNESS) * ratioSquared);
}

// Затухание: полная яркость в начале, MIN_BRIGHTNESS в конце
inline uint8_t dimSleepBrightness(const Settings &s, uint16_t elapsed) {
  uint16_t left = (elapsed >= s.sleepLength) ? 0 : (uint16_t)(s.sleepLength - elapsed);
  return curveBrightness(s.signalBrightness, squaredRatio(left, s.sleepLength));
}

// Рассвет: MIN_BRIGHTNESS в начале, максимум к концу разгорания.
// После разгорания держим максимум ещё sunLength минут.
inline uint8_t dimSunriseBrightness(const Settings &s, uint16_t elapsed) {
  return curveBrightness(s.sunriseBrightness, squaredRatio(elapsed, s.sunriseLength));
}

// ==================== Главное правило ====================
//
// blinkOn — фаза мигания, её задаёт вызывающий: логика не знает,
// сколько прошло миллисекунд, и не должна зависеть от millis().
inline Result compute(const Settings &s, uint16_t nowHhmm,
                      bool manualOn, bool blinkOn) {
  Result r;

  if (manualOn) {
    r.brightness = s.manualBrightness;
    r.mode = MODE_MANUAL;
    return r;
  }

  if (s.flags & FLAG_SLEEP) {
    uint16_t elapsed = minutesSince(s.sleepTime, nowHhmm);
    if (elapsed <= s.sleepLength) {
      // Вспышки идут первые SIGNAL_MINUTES минут интервала, то есть
      // ровно в назначенное время отхода ко сну, а не до него.
      if ((s.flags & FLAG_SIGNAL) && elapsed < SIGNAL_MINUTES) {
        r.brightness = blinkOn ? s.signalBrightness : 0;
        r.mode = MODE_SIGNAL;
        return r;
      }
      if (s.flags & FLAG_DIM) {
        r.brightness = dimSleepBrightness(s, elapsed);
        r.mode = MODE_DIM;
        return r;
      }
      r.brightness = s.manualBrightness;
      r.mode = MODE_SLEEP;
      return r;
    }
  }

  if (s.flags & FLAG_SUNRISE) {
    uint16_t elapsed = minutesSince(s.sunriseTime, nowHhmm);
    uint16_t total = (uint16_t)s.sunriseLength + (uint16_t)s.sunLength;
    if (elapsed <= total) {
      r.brightness = dimSunriseBrightness(s, elapsed);
      r.mode = MODE_SUNRISE;
      return r;
    }
  }

  r.brightness = 0;
  r.mode = MODE_OFF;
  return r;
}

}  // namespace nl

#endif
