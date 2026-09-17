/******************************************************************
 * Тесты логики светильника. Запуск: pio test -e native
 *
 * NightLight.h не зависит от Arduino, поэтому собирается обычным
 * хостовым компилятором — заглушки не нужны.
 ******************************************************************/

#include <unity.h>
#include "NightLight.h"

using namespace nl;

static Settings defaults() {
  Settings s;
  s.flags = 0;
  s.manualBrightness = 160;
  s.sleepTime = 2200;
  s.sleepLength = 90;
  s.signalBrightness = 255;
  s.sunriseTime = 700;
  s.sunriseLength = 60;
  s.sunLength = 30;
  s.sunriseBrightness = 255;
  return s;
}

// ==================== Арифметика времени ====================

void test_minutes_since_same_day() {
  TEST_ASSERT_EQUAL_UINT16(30, minutesSince(2200, 2230));
  TEST_ASSERT_EQUAL_UINT16(0, minutesSince(2200, 2200));
  TEST_ASSERT_EQUAL_UINT16(90, minutesSince(2200, 2330));
}

// Интервал через полночь был отдельной веткой в checkTimespan и
// источником путаницы. Теперь это то же вычитание по модулю суток.
void test_minutes_since_over_midnight() {
  TEST_ASSERT_EQUAL_UINT16(90, minutesSince(2300, 30));
  TEST_ASSERT_EQUAL_UINT16(1439, minutesSince(1, 0));
}

void test_hhmm_roundtrip() {
  TEST_ASSERT_EQUAL_UINT16(1350, minutesToHhmm(hhmmToMinutes(1350)));
  TEST_ASSERT_EQUAL_UINT16(0, minutesToHhmm(hhmmToMinutes(0)));
  TEST_ASSERT_EQUAL_UINT16(2359, minutesToHhmm(hhmmToMinutes(2359)));
}

// ==================== Ручное включение ====================

void test_manual_overrides_everything() {
  Settings s = defaults();
  s.flags = FLAG_SLEEP | FLAG_SIGNAL | FLAG_DIM | FLAG_SUNRISE;
  Result r = compute(s, 2200, true, false);
  TEST_ASSERT_EQUAL(MODE_MANUAL, r.mode);
  TEST_ASSERT_EQUAL_UINT8(160, r.brightness);
}

void test_all_modes_off_gives_darkness() {
  Settings s = defaults();
  Result r = compute(s, 2230, false, true);
  TEST_ASSERT_EQUAL(MODE_OFF, r.mode);
  TEST_ASSERT_EQUAL_UINT8(0, r.brightness);
}

// ==================== Отход ко сну ====================

void test_signal_blinks_at_bedtime() {
  Settings s = defaults();
  s.flags = FLAG_SLEEP | FLAG_SIGNAL;

  Result on = compute(s, 2200, false, true);
  TEST_ASSERT_EQUAL(MODE_SIGNAL, on.mode);
  TEST_ASSERT_EQUAL_UINT8(255, on.brightness);

  Result off = compute(s, 2200, false, false);
  TEST_ASSERT_EQUAL(MODE_SIGNAL, off.mode);
  TEST_ASSERT_EQUAL_UINT8(0, off.brightness);
}

// Сигнал занимает ровно SIGNAL_MINUTES минут от начала интервала
void test_signal_ends_after_five_minutes() {
  Settings s = defaults();
  s.flags = FLAG_SLEEP | FLAG_SIGNAL;
  TEST_ASSERT_EQUAL(MODE_SIGNAL, compute(s, 2204, false, true).mode);
  TEST_ASSERT_NOT_EQUAL(MODE_SIGNAL, compute(s, 2205, false, true).mode);
}

void test_dim_falls_from_full_to_minimum() {
  Settings s = defaults();
  s.flags = FLAG_SLEEP | FLAG_DIM;

  // Начало интервала перекрыто сигналом, поэтому берём момент после
  // него: яркость уже ниже стартовой, но ещё далека от минимума.
  Result early = compute(s, 2210, false, true);
  TEST_ASSERT_EQUAL(MODE_DIM, early.mode);

  Result late = compute(s, 2320, false, true);
  TEST_ASSERT_EQUAL(MODE_DIM, late.mode);
  TEST_ASSERT_TRUE(late.brightness < early.brightness);

  // В конце интервала светильник тлеет, но не гаснет
  Result end = compute(s, 2330, false, true);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)MIN_BRIGHTNESS, end.brightness);
}

void test_sleep_interval_ends() {
  Settings s = defaults();
  s.flags = FLAG_SLEEP | FLAG_DIM;
  TEST_ASSERT_EQUAL(MODE_DIM, compute(s, 2330, false, true).mode);
  TEST_ASSERT_EQUAL(MODE_OFF, compute(s, 2331, false, true).mode);
}

// Засыпание с 23:30 уходит за полночь — интервал должен продолжаться
void test_sleep_interval_over_midnight() {
  Settings s = defaults();
  s.flags = FLAG_SLEEP | FLAG_DIM;
  s.sleepTime = 2330;
  s.sleepLength = 60;
  TEST_ASSERT_EQUAL(MODE_DIM, compute(s, 15, false, true).mode);
  TEST_ASSERT_EQUAL(MODE_DIM, compute(s, 30, false, true).mode);
  TEST_ASSERT_EQUAL(MODE_OFF, compute(s, 31, false, true).mode);
}

// Без затухания режим сна держит обычную яркость
void test_sleep_without_dim_holds_brightness() {
  Settings s = defaults();
  s.flags = FLAG_SLEEP;
  Result r = compute(s, 2230, false, true);
  TEST_ASSERT_EQUAL(MODE_SLEEP, r.mode);
  TEST_ASSERT_EQUAL_UINT8(160, r.brightness);
}

// ==================== Рассвет ====================

void test_sunrise_grows_to_maximum() {
  Settings s = defaults();
  s.flags = FLAG_SUNRISE;

  Result start = compute(s, 700, false, true);
  TEST_ASSERT_EQUAL(MODE_SUNRISE, start.mode);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)MIN_BRIGHTNESS, start.brightness);

  Result middle = compute(s, 730, false, true);
  TEST_ASSERT_TRUE(middle.brightness > start.brightness);

  Result end = compute(s, 800, false, true);
  TEST_ASSERT_EQUAL_UINT8(255, end.brightness);
}

// Прежняя версия скармливала параболе время за пределами разгорания,
// и при максимуме ниже 255 яркость уползала выше заданной.
void test_sunrise_holds_configured_maximum_after_ramp() {
  Settings s = defaults();
  s.flags = FLAG_SUNRISE;
  s.sunriseBrightness = 100;

  TEST_ASSERT_EQUAL_UINT8(100, compute(s, 800, false, true).brightness);
  TEST_ASSERT_EQUAL_UINT8(100, compute(s, 815, false, true).brightness);
  TEST_ASSERT_EQUAL_UINT8(100, compute(s, 830, false, true).brightness);
  TEST_ASSERT_EQUAL(MODE_OFF, compute(s, 831, false, true).mode);
}

// Сон имеет приоритет: пересечение интервалов не должно давать
// рассвет посреди засыпания
void test_sleep_wins_over_sunrise_on_overlap() {
  Settings s = defaults();
  s.flags = FLAG_SLEEP | FLAG_DIM | FLAG_SUNRISE;
  s.sleepTime = 700;
  s.sleepLength = 30;
  TEST_ASSERT_EQUAL(MODE_DIM, compute(s, 710, false, true).mode);
}

// ==================== Вырожденные настройки ====================

// Длительность рассвета ровно 2 минуты обращала знаменатель прежней
// формулы в ноль, и яркость становилась NaN
void test_two_minute_sunrise_is_finite() {
  Settings s = defaults();
  s.flags = FLAG_SUNRISE;
  s.sunriseLength = 2;
  s.sunLength = 0;
  Result r = compute(s, 701, false, true);
  TEST_ASSERT_EQUAL(MODE_SUNRISE, r.mode);
  TEST_ASSERT_TRUE(r.brightness >= (uint8_t)MIN_BRIGHTNESS);
  TEST_ASSERT_TRUE(r.brightness <= 255);
}

void test_zero_length_intervals_do_not_divide_by_zero() {
  Settings s = defaults();
  s.flags = FLAG_SLEEP | FLAG_DIM | FLAG_SUNRISE;
  s.sleepLength = 0;
  s.sunriseLength = 0;
  s.sunLength = 0;

  Result sleep = compute(s, 2200, false, true);
  TEST_ASSERT_EQUAL(MODE_DIM, sleep.mode);
  TEST_ASSERT_TRUE(sleep.brightness <= 255);

  Result sunrise = compute(s, 700, false, true);
  TEST_ASSERT_EQUAL(MODE_SUNRISE, sunrise.mode);
  TEST_ASSERT_EQUAL_UINT8(255, sunrise.brightness);
}

// Яркость ниже порога тления поднимается до него, а не гасит лампу
void test_brightness_below_minimum_is_raised() {
  Settings s = defaults();
  s.flags = FLAG_SLEEP | FLAG_DIM;
  s.signalBrightness = 1;
  Result r = compute(s, 2210, false, true);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)MIN_BRIGHTNESS, r.brightness);
}

// Unity требует эти две функции даже пустыми: они вызываются вокруг
// каждого теста, а общего состояния здесь нет.
void setUp(void) {}
void tearDown(void) {}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_minutes_since_same_day);
  RUN_TEST(test_minutes_since_over_midnight);
  RUN_TEST(test_hhmm_roundtrip);
  RUN_TEST(test_manual_overrides_everything);
  RUN_TEST(test_all_modes_off_gives_darkness);
  RUN_TEST(test_signal_blinks_at_bedtime);
  RUN_TEST(test_signal_ends_after_five_minutes);
  RUN_TEST(test_dim_falls_from_full_to_minimum);
  RUN_TEST(test_sleep_interval_ends);
  RUN_TEST(test_sleep_interval_over_midnight);
  RUN_TEST(test_sleep_without_dim_holds_brightness);
  RUN_TEST(test_sunrise_grows_to_maximum);
  RUN_TEST(test_sunrise_holds_configured_maximum_after_ramp);
  RUN_TEST(test_sleep_wins_over_sunrise_on_overlap);
  RUN_TEST(test_two_minute_sunrise_is_finite);
  RUN_TEST(test_zero_length_intervals_do_not_divide_by_zero);
  RUN_TEST(test_brightness_below_minimum_is_raised);
  return UNITY_END();
}
