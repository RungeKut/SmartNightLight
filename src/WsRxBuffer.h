#pragma once
#include <Arduino.h>

/*
 * Сборка входящего WebSocket-сообщения из кусков.
 *
 * ЗАЧЕМ
 *
 * ESPAsyncWebServer отдаёт данные так, как они пришли по TCP. Сообщение
 * длиннее одного сегмента (на ESP8266 это около 536 байт) приезжает
 * несколькими вызовами WS_EVT_DATA, а длинное — ещё и несколькими
 * кадрами с opcode CONTINUATION.
 *
 * Прежний обработчик принимал сообщение, только если оно уложилось в
 * один вызов:
 *
 *   if (info->final && info->index == 0 && info->len == len && ...)
 *
 * Пока saveConfig весил около 500 байт, это работало. Когда в конфиг
 * добавились поля защиты фильтра, сообщение выросло до ~700 байт,
 * условие перестало выполняться — и настройки молча перестали
 * сохраняться. Ни ошибки, ни записи в лог: ветки else просто не было.
 *
 * ПОЧЕМУ ФИКСИРОВАННЫЙ БУФЕР, А НЕ String
 *
 * На этой плате куча фрагментируется (см. pitfalls.md §29), и растущая
 * строка на каждое входящее сообщение — лишний источник измельчения там,
 * где сразу следом ArduinoJson просит крупный непрерывный кусок.
 * Статический буфер стоит WSRX_MAX_BYTES байт один раз за всё время.
 *
 * ЧУЖОЙ КУСОК
 *
 * Клиентов может быть несколько, и их сообщения теоретически способны
 * перемешаться. Сборка привязана к идентификатору клиента: кусок от
 * другого клиента посреди набора отбрасывается, а не дописывается в
 * чужое сообщение. Счётчик drops() показывает, что такое случалось.
 */

#define WSRX_MAX_BYTES     1536   // saveConfig сейчас ~700 байт
#define WSRX_OPCODE_CONT   0
#define WSRX_OPCODE_TEXT   1

class WsRxBuffer {
private:
  char     _buf[WSRX_MAX_BYTES];
  uint32_t _len;
  uint32_t _client;
  bool     _active;
  uint32_t _drops;

public:
  WsRxBuffer() : _len(0), _client(0), _active(false), _drops(0) { _buf[0] = '\0'; }

  void reset() {
    _len = 0;
    _active = false;
    _buf[0] = '\0';
  }

  // Принять очередной кусок. Возвращает true, когда сообщение собрано
  // целиком — тогда message() указывает на готовую строку.
  //
  //   opcode   — код ТЕКУЩЕГО кадра (TEXT или CONTINUATION)
  //   final    — бит FIN этого кадра
  //   index    — смещение куска внутри кадра
  //   len      — длина этого куска
  //   frameLen — полная длина кадра
  bool feed(uint32_t clientId, uint8_t opcode, bool final,
            uint32_t index, uint32_t len, uint32_t frameLen,
            const uint8_t *data) {
    // Начало нового сообщения: текстовый кадр с нулевого смещения
    if (opcode == WSRX_OPCODE_TEXT && index == 0) {
      reset();
      _client = clientId;
      _active = true;
    } else if (!_active || _client != clientId) {
      // Продолжение без начала или кусок от другого клиента —
      // дописывать его в чужое сообщение нельзя
      _drops++;
      return false;
    }

    if (_len + len >= WSRX_MAX_BYTES) {
      // Сообщение не влезло. Молча обрезать хуже, чем отказаться:
      // обрезанный JSON разобрался бы с ошибкой и увёл бы диагностику
      // не туда.
      _drops++;
      reset();
      return false;
    }

    if (len > 0 && data != nullptr) {
      memcpy(_buf + _len, data, len);
      _len += len;
    }
    _buf[_len] = '\0';

    // Кадр дочитан и он последний — сообщение готово
    if (final && (index + len) >= frameLen) {
      _active = false;
      return true;
    }
    return false;
  }

  const char* message() const { return _buf; }
  uint32_t    length()  const { return _len; }
  uint32_t    drops()   const { return _drops; }
};
