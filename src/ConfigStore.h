/******************************************************************
 * ConfigStore.h — настройки в EEPROM
 *
 * Первая версия прошивки писала поля по фиксированным адресам
 * (EEPROM.get(2, sleepTime) и так далее) без всякой отметки о том,
 * инициализирована ли память. На чистой плате оттуда читались 0xFF,
 * и sleepTime становился 65535 — дефолты из кода затирались мусором
 * ещё до первого запуска.
 *
 * Здесь всё лежит одной структурой с магическим числом. Не совпало —
 * настройки считаются отсутствующими и берутся значения по умолчанию.
 *
 * РАСШИРЕНИЕ СТРУКТУРЫ: новые поля добавлять ТОЛЬКО В КОНЕЦ. Тогда
 * смещения прежних полей не меняются, magic остаётся валидным, и
 * настройки переживают обновление прошивки. В байтах за прежним
 * размером лежит 0xFF из стёртой flash, поэтому каждое новое поле
 * должно уметь распознать это значение как "не задано".
 * Если порядок или размер существующих полей всё же меняется —
 * обязательно меняйте и EEPROM_MAGIC.
 ******************************************************************/

#ifndef ConfigStore_h
#define ConfigStore_h

#include <Arduino.h>
#include <EEPROM.h>
#include "Log.h"
#include "NightLight.h"

#define EEPROM_MAGIC 0x4E4C   // 'NL'

struct ConfigData {
  uint16_t magic;

  // WiFi. Пустой SSID = устройство поднимает точку доступа.
  char wifiSSID[32];
  char wifiPass[64];

  // Имя устройства: hostname, mDNS, SSID точки доступа.
  // Пустое = сгенерировать из MAC: SmartNightLight-XXXXXX.
  char deviceName[32];

  // Режимы светильника (маска nl::FLAG_*)
  uint8_t  flags;
  uint8_t  manualBrightness;

  uint16_t sleepTime;          // hhmm
  uint8_t  sleepLength;        // минут
  uint8_t  signalBrightness;

  uint16_t sunriseTime;        // hhmm
  uint8_t  sunriseLength;      // минут
  uint8_t  sunLength;          // минут
  uint8_t  sunriseBrightness;

  // Часовой пояс в минутах от UTC. Был зашит как +3 в двух местах
  // сразу (константа NTP_OFFSET и setTimeOffset), теперь настройка.
  int16_t  tzOffsetMinutes;

  uint8_t  _reserved[16];      // место под новые поля без сдвига старых
};

class ConfigStore {
public:
  ConfigData data;

  void begin() {
    EEPROM.begin(sizeof(ConfigData));
    EEPROM.get(0, data);

    if (data.magic != EEPROM_MAGIC) {
      Log.printf("[Config] EEPROM пуста или чужая (magic=0x%04X), беру умолчания\n",
                 data.magic);
      setDefaults();
      return;
    }
    sanitize();
    Log.printf("[Config] Загружено, SSID: '%s'\n",
               strlen(data.wifiSSID) ? data.wifiSSID : "(не задан)");
  }

  void save() {
    data.magic = EEPROM_MAGIC;
    // EEPROM.put() в ядре ESP8266 сравнивает буфер с текущим
    // содержимым и выставляет _dirty только при реальном отличии, а
    // commit() при !_dirty выходит сразу. Поэтому сохранение без
    // изменений физически ничего не пишет и ресурс flash не тратит.
    EEPROM.put(0, data);
    if (EEPROM.commit()) {
      Log.println(F("[Config] Сохранено"));
    } else {
      Log.println(F("[Config] Ошибка записи EEPROM"));
    }
  }

  void setDefaults() {
    memset(&data, 0, sizeof(data));
    data.magic = EEPROM_MAGIC;
    data.flags = 0;
    data.manualBrightness = 160;
    data.sleepTime = 2200;          // 22:00
    data.sleepLength = 90;
    data.signalBrightness = 255;
    data.sunriseTime = 700;         // 07:00
    data.sunriseLength = 60;
    data.sunLength = 30;
    data.sunriseBrightness = 255;
    data.tzOffsetMinutes = 180;     // GMT+3, Москва
  }

  // Выдаёт настройки в том виде, в каком их ждёт логика светильника
  nl::Settings toSettings() const {
    nl::Settings s;
    s.flags = data.flags;
    s.manualBrightness = data.manualBrightness;
    s.sleepTime = data.sleepTime;
    s.sleepLength = data.sleepLength;
    s.signalBrightness = data.signalBrightness;
    s.sunriseTime = data.sunriseTime;
    s.sunriseLength = data.sunriseLength;
    s.sunLength = data.sunLength;
    s.sunriseBrightness = data.sunriseBrightness;
    return s;
  }

private:
  // Магическое число подтверждает, что память вообще инициализирована,
  // но не то, что значения осмысленны: обрыв питания посреди записи
  // или новое поле в хвосте структуры дают мусор с валидным magic.
  void sanitize() {
    terminate(data.wifiSSID, sizeof(data.wifiSSID));
    terminate(data.wifiPass, sizeof(data.wifiPass));
    terminate(data.deviceName, sizeof(data.deviceName));

    if (!isValidHhmm(data.sleepTime)) data.sleepTime = 2200;
    if (!isValidHhmm(data.sunriseTime)) data.sunriseTime = 700;

    // 0xFFFF в поле, дописанном в конец структуры, как int16_t даёт -1.
    // Настоящие смещения лежат в пределах суток.
    if (data.tzOffsetMinutes < -720 || data.tzOffsetMinutes > 840) {
      data.tzOffsetMinutes = 180;
    }
  }

  static bool isValidHhmm(uint16_t v) {
    return v <= 2359 && (v % 100) <= 59;
  }

  // Строка из EEPROM может не содержать нуля вовсе: если все байты
  // равны 0xFF, любой цикл по src[i] уйдёт читать соседнюю память.
  static void terminate(char *s, size_t size) {
    for (size_t i = 0; i < size; i++) {
      if (s[i] == '\0') return;
      if ((uint8_t)s[i] == 0xFF) { s[i] = '\0'; return; }
    }
    s[size - 1] = '\0';
  }
};

#endif
