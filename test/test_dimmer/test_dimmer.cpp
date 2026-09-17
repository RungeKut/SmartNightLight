/******************************************************************
 * Тесты кривой яркости. Запуск: pio test -e native
 ******************************************************************/

#include <unity.h>
#include "Dimmer.h"

// Ноль означает "погашен" и обязан оставаться нулём: иначе светильник
// никогда не выключится до конца
void test_zero_stays_zero() {
  Dimmer d;
  TEST_ASSERT_EQUAL_UINT16(0, d.duty(0));
}

// Любое ненулевое значение обязано что-то зажигать. Чистая степенная
// функция отправляла низ шкалы в ноль, и "еле тлеет" превращалось в
// "погашен".
void test_any_nonzero_level_lights_up() {
  Dimmer d;
  for (int i = 1; i <= 255; i++) {
    TEST_ASSERT_TRUE_MESSAGE(d.duty((uint8_t)i) >= PWM_MIN_DUTY,
                             "нижние деления ушли в ноль");
  }
}

void test_full_level_is_full_duty() {
  Dimmer d;
  TEST_ASSERT_EQUAL_UINT16(PWM_MAX_DUTY, d.duty(255));
}

// Ползунок не должен нигде идти назад
void test_curve_is_monotonic() {
  Dimmer d;
  for (uint8_t g = GAMMA_X10_MIN; g <= GAMMA_X10_MAX; g++) {
    d.setGamma(g);
    uint16_t prev = 0;
    for (int i = 0; i <= 255; i++) {
      uint16_t cur = d.duty((uint8_t)i);
      TEST_ASSERT_TRUE_MESSAGE(cur >= prev, "кривая пошла вниз");
      prev = cur;
    }
  }
}

// Смысл гаммы: середина ползунка даёт заметно меньше половины
// мощности, потому что глаз видит её как половину
void test_midpoint_is_below_linear() {
  Dimmer d;
  d.setGamma(22);
  uint16_t mid = d.duty(128);
  TEST_ASSERT_TRUE(mid < PWM_MAX_DUTY / 3);
  TEST_ASSERT_TRUE(mid > PWM_MIN_DUTY);
}

// Гамма 1.0 — это линейная шкала, как было до коррекции
void test_gamma_one_is_linear() {
  Dimmer d;
  d.setGamma(10);
  uint16_t mid = d.duty(128);
  // Примерно половина шкалы с поправкой на поднятое дно
  TEST_ASSERT_TRUE(mid > PWM_MAX_DUTY * 0.45);
  TEST_ASSERT_TRUE(mid < PWM_MAX_DUTY * 0.55);
}

// Значение вне допустимого диапазона не должно ломать таблицу
void test_gamma_is_clamped() {
  Dimmer d;
  d.setGamma(200);
  TEST_ASSERT_EQUAL_UINT8(GAMMA_X10_MAX, d.gammaX10());
  d.setGamma(1);
  TEST_ASSERT_EQUAL_UINT8(GAMMA_X10_MIN, d.gammaX10());
  TEST_ASSERT_EQUAL_UINT16(PWM_MAX_DUTY, d.duty(255));
}

// Большая гамма растягивает низ шкалы сильнее
void test_higher_gamma_dims_low_end() {
  Dimmer d;
  d.setGamma(15);
  uint16_t soft = d.duty(60);
  d.setGamma(28);
  uint16_t hard = d.duty(60);
  TEST_ASSERT_TRUE(hard < soft);
}

void setUp(void) {}
void tearDown(void) {}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_zero_stays_zero);
  RUN_TEST(test_any_nonzero_level_lights_up);
  RUN_TEST(test_full_level_is_full_duty);
  RUN_TEST(test_curve_is_monotonic);
  RUN_TEST(test_midpoint_is_below_linear);
  RUN_TEST(test_gamma_one_is_linear);
  RUN_TEST(test_gamma_is_clamped);
  RUN_TEST(test_higher_gamma_dims_low_end);
  return UNITY_END();
}
