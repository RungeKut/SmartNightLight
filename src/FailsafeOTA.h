/******************************************************************
 * FailsafeOTA.h - Контроль успешности OTA-обновления
 *
 * ЧТО ЭТО НА САМОМ ДЕЛЕ
 *
 * На ESP8266 нет A/B-слотов. Штатный механизм Arduino-ядра:
 *   1. ArduinoOTA пишет новый образ во временную область flash
 *      (сразу под файловой системой)
 *   2. Updater::end() пишет в RTC команду eboot ACTION_COPY_RAW
 *   3. При перезагрузке загрузчик eboot КОПИРУЕТ образ в 0x00000
 *      поверх старой прошивки и стартует его
 * Старой прошивки после этого не существует — откатываться некуда.
 * Автоматический откат от неработающей прошивки в принципе возможен
 * только с заменой самого загрузчика eboot, что прошивается лишь
 * по USB (см. docs/modules/ota.md).
 *
 * ЧТО ДЕЛАЕТ ЭТОТ МОДУЛЬ
 *
 * Даёт видимый признак того, что новая прошивка действительно
 * дожила до loop() и принимает команды:
 *   1. ArduinoOTA.onEnd() -> updateFirmware(): флаг в RTC memory
 *   2. Новая прошивка стартует, begin() видит флаг -> _pending
 *   3. В веб-интерфейсе появляется баннер с обратным отсчётом
 *   4. Пользователь подтверждает: /confirm, кнопка в SPA или
 *      команда confirm в Telnet
 *   5. Нет подтверждения за 5 минут -> перезагрузка (вдруг
 *      прошивка «полуживая» и рестарт вернёт её в строй)
 *   6. Если и после перезагрузки подтверждения не было — модуль
 *      прекращает попытки и просто пишет предупреждение в лог,
 *      чтобы не уйти в бесконечный цикл ребутов
 *
 * Использование:
 *   1. FailsafeOTA::begin() в setup()
 *   2. FailsafeOTA::updateFirmware() в ArduinoOTA.onEnd()
 *   3. FailsafeOTA::confirm() при подтверждении
 *   4. FailsafeOTA::isPending() / remainingSec() — для баннера
 *   5. FailsafeOTA::handle() в loop()
 ******************************************************************/

#ifndef FailsafeOTA_h
#define FailsafeOTA_h

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include "Log.h"

// Флаги в RTC memory (переживают soft-reset, но не потерю питания)
#define RTC_MAGIC_FIRST_BOOT  0xDEAD0001  // первый старт после OTA
#define RTC_MAGIC_RETRY       0xDEAD0002  // старт после перезагрузки по таймауту
#define RTC_MAGIC_NORMAL      0x00000000  // обычный старт

// КРИТИЧНО: смещение в пользовательской RTC-памяти (в 4-байтовых блоках).
//
// Первые 128 байт (блоки 0..31) занимает команда загрузчика eboot, которую
// Updater::end() записывает при OTA. Цитата из ядра ESP8266 (Esp.cpp):
//
//   "If the Updater class is in play, e.g.: the application uses OTA, the
//    eboot command will be stored into the first 128 bytes of user data,
//    then it will be retrieved by eboot on boot. That means that user data
//    present there will be lost."
//
// Порядок вызовов в ArduinoOTA::_runUpdate():
//   1. Update.end()      -> пишет команду eboot ACTION_COPY_RAW
//   2. _end_callback()   -> наш updateFirmware()
//   3. ESP.restart()
//
// Запись флага в блок 0 затирала магическое число команды eboot. Загрузчик
// не находил валидной команды, образ из временной области не копировался, и
// плата поднималась на СТАРОЙ прошивке — при этом OTA рапортовал "Result: OK".
// Именно поэтому OTA в этом проекте никогда не применялся.
#define RTC_FLAG_OFFSET       32          // блок 32 = байт 128, сразу за eboot

class FailsafeOTA {
private:
  uint32_t _bootTime;
  uint32_t _confirmTimeout;     // мс до перезагрузки (5 минут)
  bool _pending;                // ждём подтверждения
  bool _confirmed;              // пользователь подтвердил
  bool _retryExhausted;         // перезагрузка уже была, больше не пробуем

  static void writeRTC(uint32_t val) {
    ESP.rtcUserMemoryWrite(RTC_FLAG_OFFSET, &val, sizeof(val));
  }

  static uint32_t readRTC() {
    uint32_t val = 0;
    ESP.rtcUserMemoryRead(RTC_FLAG_OFFSET, &val, sizeof(val));
    return val;
  }

public:
  FailsafeOTA() : _bootTime(0), _confirmTimeout(300000),
                  _pending(false), _confirmed(false),
                  _retryExhausted(false) {}

  void begin() {
    _bootTime = millis();
    uint32_t rtcVal = readRTC();

    Log.printf("[Failsafe] RTC=0x%08X\n", rtcVal);

    if (rtcVal == RTC_MAGIC_FIRST_BOOT) {
      // Первый старт после OTA — ждём подтверждения
      _pending = true;
      _confirmed = false;
      _retryExhausted = false;
      Log.printf("[Failsafe] New firmware booted. Confirm within %u s\n",
        _confirmTimeout / 1000);
      writeRTC(RTC_MAGIC_RETRY);

    } else if (rtcVal == RTC_MAGIC_RETRY) {
      // Мы уже перезагружались из-за отсутствия подтверждения.
      // Второй раз не перезагружаемся — иначе получится bootloop.
      _pending = true;
      _confirmed = false;
      _retryExhausted = true;
      Log.println("[Failsafe] Firmware still unconfirmed after restart.");
      Log.println("[Failsafe] Reflash via USB if the device misbehaves.");
      writeRTC(RTC_MAGIC_NORMAL);

    } else {
      // Обычный старт
      _pending = false;
      _confirmed = true;
      writeRTC(RTC_MAGIC_NORMAL);
    }
  }

  // Вызывается из loop() — проверяет таймаут подтверждения
  void handle() {
    if (!_pending || _confirmed || _retryExhausted) return;

    if (millis() - _bootTime > _confirmTimeout) {
      Log.println("[Failsafe] Confirmation timeout — restarting once...");
      delay(100);
      ESP.restart();
    }
  }

  // ВЫЗЫВАТЬ ПОСЛЕ УСПЕШНОГО OTA (в ArduinoOTA.onEnd)
  void updateFirmware() {
    writeRTC(RTC_MAGIC_FIRST_BOOT);
    Log.println("[Failsafe] OTA done — waiting for confirmation on next boot");
  }

  // Подтверждение новой прошивки (веб-интерфейс или Telnet)
  void confirm() {
    if (_pending) {
      _confirmed = true;
      _pending = false;
      _retryExhausted = false;
      writeRTC(RTC_MAGIC_NORMAL);
      Log.println("[Failsafe] Firmware confirmed!");
    }
  }

  bool isPending() { return _pending && !_confirmed; }

  uint32_t remainingSec() {
    if (!_pending || _confirmed || _retryExhausted) return 0;
    uint32_t elapsed = (millis() - _bootTime) / 1000;
    uint32_t total = _confirmTimeout / 1000;
    return (elapsed < total) ? (total - elapsed) : 0;
  }
};

#endif
