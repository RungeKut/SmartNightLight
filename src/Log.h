/******************************************************************
 * Log.h - Единая точка вывода логов
 *
 * Проблема, которую решает модуль:
 *   Serial.print/printf пишут напрямую в UART. Хук os_install_putc1()
 *   перехватывает только вывод SDK (os_printf), но НЕ вывод скетча,
 *   поэтому логи прошивки в Telnet не попадали.
 *
 * Решение:
 *   Log — наследник Print. Каждый байт уходит и в Serial, и в
 *   зарегистрированный приёмник (TelnetSerial).
 *
 * Использование:
 *   Log.printf("[DS18B20] Found: %d\n", n);   // вместо Serial.printf
 *   Log.setSink(&telnet);                      // в setup()
 *
 * Низкоуровневый вывод SDK (сообщения WiFi-стека) остаётся только
 * на USB-Serial — писать в TCP из контекста SDK небезопасно.
 ******************************************************************/

#ifndef Log_h
#define Log_h

#include <Arduino.h>

// Приёмник логов (реализуется в TelnetSerial)
class LogSink {
public:
  virtual void logWrite(const uint8_t *buf, size_t size) = 0;
};

class Logger : public Print {
private:
  LogSink *_sink;

public:
  Logger() : _sink(nullptr) {}

  void setSink(LogSink *sink) { _sink = sink; }

  size_t write(uint8_t c) override {
    Serial.write(c);
    if (_sink) _sink->logWrite(&c, 1);
    return 1;
  }

  size_t write(const uint8_t *buf, size_t size) override {
    Serial.write(buf, size);
    if (_sink) _sink->logWrite(buf, size);
    return size;
  }
};

// Проект собирается как одна единица трансляции (все модули —
// header-only, включаются из Wemos_Mini.ino), поэтому определение
// глобального объекта в заголовке безопасно.
Logger Log;

#endif
