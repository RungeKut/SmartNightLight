/******************************************************************
 * MqttClient.h — MQTT и автодискавери Home Assistant
 *
 * ЗАЧЕМ, ЕСЛИ УЖЕ ЕСТЬ /api.json
 *
 * REST требует, чтобы Home Assistant опрашивал устройство и чтобы
 * сущности были описаны руками в YAML. MQTT переворачивает схему:
 * устройство само публикует данные в брокер и само себя описывает —
 * сущности появляются в HA без единой строки конфигурации.
 *
 * REST и /metrics при этом остаются: они работают без брокера и
 * удобны для curl и Prometheus.
 *
 * ПОЧЕМУ AsyncMqttClient, А НЕ PubSubClient
 *
 * PubSubClient::connect() блокирует loop() до таймаута сокета:
 * недоступный брокер — и светильник дёргается раз в цикл. У нас в
 * loop() живёт PWM и опрос микрофона, вставать нельзя.
 * AsyncMqttClient работает поверх ESPAsyncTCP, который уже есть.
 *
 * СТРУКТУРА ТЕМ
 *
 *   smartnightlight/<MAC3>/state     телеметрия одним JSON
 *   smartnightlight/<MAC3>/light     состояние лампы (JSON-схема HA)
 *   smartnightlight/<MAC3>/status    online / offline (LWT)
 *   smartnightlight/<MAC3>/cmd/+     команды от Home Assistant
 *   homeassistant/<тип>/snl<MAC3>/<объект>/config   автодискавери
 *
 * Базовая тема и идентификаторы строятся от MAC, а НЕ от имени
 * устройства: имя пользователь может поменять, и тогда HA потерял бы
 * связь с уже созданными сущностями. Понятное имя уходит в
 * device.name автодискавери.
 *
 * ЧТО ОЗНАЧАЕТ ЛАМПА В HA
 *
 * Сущность light управляет РУЧНЫМ включением, а не подменяет собой
 * расписание. Включили из HA — светильник горит поверх расписания,
 * выключили — вернулся к своим режимам. Фактическая яркость и
 * текущий режим отдаются отдельными сенсорами, чтобы было видно,
 * чем именно светильник занят.
 ******************************************************************/

#ifndef MqttClient_h
#define MqttClient_h

#include <Arduino.h>
#include <AsyncMqttClient.h>
#include <ArduinoJson.h>
#include "Log.h"
#include "ConfigStore.h"

#define MQTT_RECONNECT_MS      30000
#define MQTT_DEFAULT_INTERVAL  5       // период публикации состояния, с
#define MQTT_DISCOVERY_PREFIX  "homeassistant"

// Описания сущностей публикуем по одному за проход loop(): полтора
// десятка сообщений подряд по ~400 байт исчерпали бы буферы TCP.
#define MQTT_DISCOVERY_STEP_MS 120

class MqttClient {
public:
  // Что устройство отдаёт наружу. Заполняется в Wemos_Mini.ino,
  // чтобы модуль не зависел от остального проекта.
  struct Payload {
    uint8_t  brightness;     // фактическая яркость 0..255
    const char *mode;        // off / manual / dim / sunrise / sound / ...
    bool     manualOn;
    uint8_t  manualBrightness;

    uint16_t soundLevel;     // текущий размах с микрофона
    uint16_t soundPeak;      // пик за последнюю минуту
    uint32_t soundTriggers;     // событий внутри ночного окна
    uint32_t soundTriggersOut;  // событий за его пределами
    bool     soundActive;    // отклик на шум идёт прямо сейчас

    bool     sleepEnabled, sunriseEnabled, soundEnabled;
    bool     timeValid;

    int32_t  rssi;
    uint32_t uptimeSec;
    uint32_t freeHeap;
  };

  typedef void (*CommandCallback)(const String &cmd, const String &value);

private:
  AsyncMqttClient _mqtt;
  ConfigStore *_config;
  CommandCallback _cb;

  char _id[16];          // snlB92046
  char _base[40];        // smartnightlight/B92046
  char _friendly[32];
  char _swVersion[24];

  bool _enabled;
  bool _connected;
  uint32_t _lastAttempt;
  uint32_t _lastPublish;
  int _discoveryStep;
  uint32_t _lastDiscoveryMs;

  uint32_t _publishCount;
  uint32_t _reconnects;
  int8_t _lastError;     // -1 = отключений не было

  static MqttClient *_instance;

  uint32_t intervalMs() const {
    uint16_t s = _config ? _config->data.mqttIntervalSec : 0;
    if (s == 0) s = MQTT_DEFAULT_INTERVAL;
    return (uint32_t)s * 1000UL;
  }

  // ---- автодискавери ----

  struct EntityDef {
    const char *component;   // sensor / binary_sensor / switch / button / number / light
    const char *object;
    const char *name;
    const char *valueKey;    // путь в state JSON, пусто для button и light
    const char *deviceClass;
    const char *stateClass;
    const char *unit;
    const char *category;    // diagnostic / config / nullptr
    const char *command;     // суффикс cmd-темы
    const char *extra;       // доп. поля JSON
  };

  static const EntityDef *entities(int &count) {
    static const EntityDef defs[] = {
      // --- лампа ---
      // JSON-схема: одно сообщение несёт и состояние, и яркость.
      // Отдельная тема light, а не общий state: HA ждёт здесь строго
      // свой формат, лишние поля рядом ему мешать не должны.
      { "light", "lamp", "Ночник", nullptr,
        nullptr, nullptr, nullptr, nullptr, "light",
        "\"schema\":\"json\",\"brightness\":true,\"brightness_scale\":255" },

      // --- что происходит ---
      { "sensor", "brightness", "Яркость", "brightness",
        nullptr, "measurement", nullptr, nullptr, nullptr, nullptr },
      { "sensor", "mode", "Режим", "mode",
        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr },

      // --- шум ---
      // Ради этого сенсора всё и затевалось: график ночной активности.
      { "sensor", "sound_level", "Уровень шума", "sound.level",
        nullptr, "measurement", nullptr, nullptr, nullptr, nullptr },
      // Пик и счётчик срабатываний НЕ diagnostic: категория diagnostic
      // прячет сущность в отдельный раздел карточки, а это ровно те две
      // величины, ради которых всё затевалось — ночная активность на
      // графике. Уровень измеряется круглосуточно и публикуется всегда,
      // независимо от ночного окна и от того, включён ли режим.
      { "sensor", "sound_peak", "Пик шума за минуту", "sound.peak",
        nullptr, "measurement", nullptr, nullptr, nullptr, nullptr },
      { "sensor", "sound_triggers", "Шум в ночном окне", "sound.triggers",
        nullptr, "total_increasing", nullptr, nullptr, nullptr, nullptr },
      { "sensor", "sound_triggers_out", "Шум вне ночного окна", "sound.triggers_out",
        nullptr, "total_increasing", nullptr, nullptr, nullptr, nullptr },
      // device_class sound: в HA такая сущность читается как
      // "обнаружен звук" и годится в триггер автоматизации
      { "binary_sensor", "sound_active", "Отклик на шум", "sound.active",
        "sound", nullptr, nullptr, nullptr, nullptr, nullptr },

      // --- режимы ---
      { "switch", "mode_sleep", "Режим отхода ко сну", "modes.sleep",
        nullptr, nullptr, nullptr, "config", "mode_sleep", nullptr },
      { "switch", "mode_sunrise", "Имитация рассвета", "modes.sunrise",
        nullptr, nullptr, nullptr, "config", "mode_sunrise", nullptr },
      { "switch", "mode_sound", "Реакция на шум", "modes.sound",
        nullptr, nullptr, nullptr, "config", "mode_sound", nullptr },

      // Яркость ручного режима отдельной сущностью, а не только
      // ползунком лампы: у выключенной лампы HA отдаёт brightness=None,
      // то есть заданное значение не видно и задать его заранее нельзя.
      // Через number его видно всегда и можно выставить, не зажигая свет.
      { "number", "manual_brightness", "Яркость ручного режима", "cfg.manual_brightness",
        nullptr, nullptr, nullptr, "config", "manual_brightness",
        "\"min\":0,\"max\":255,\"step\":1,\"mode\":\"slider\"" },

      { "number", "sound_threshold", "Порог шума", "cfg.sound_threshold",
        nullptr, nullptr, nullptr, "config", "sound_threshold",
        "\"min\":0,\"max\":1023,\"step\":1,\"mode\":\"box\"" },
      { "number", "sound_brightness", "Яркость отклика", "cfg.sound_brightness",
        nullptr, nullptr, nullptr, "config", "sound_brightness",
        "\"min\":0,\"max\":255,\"step\":1,\"mode\":\"box\"" },

      // --- диагностика ---
      { "sensor", "rssi", "WiFi RSSI", "sys.rssi",
        "signal_strength", "measurement", "dBm", "diagnostic", nullptr, nullptr },
      { "sensor", "uptime", "Аптайм", "sys.uptime",
        "duration", "measurement", "s", "diagnostic", nullptr, nullptr },
      { "sensor", "free_heap", "Свободная память", "sys.heap",
        "data_size", "measurement", "B", "diagnostic", nullptr, nullptr },
      { "binary_sensor", "time_valid", "Время синхронизировано", "sys.time_valid",
        nullptr, nullptr, nullptr, "diagnostic", nullptr, nullptr },

      // --- управление ---
      { "button", "restart", "Перезагрузить", nullptr,
        "restart", nullptr, nullptr, "config", "restart", nullptr },
      { "button", "confirm_ota", "Подтвердить прошивку", nullptr,
        nullptr, nullptr, nullptr, "config", "confirm_ota", nullptr },
    };
    count = sizeof(defs) / sizeof(defs[0]);
    return defs;
  }

  void publishDiscoveryEntity(const EntityDef &e) {
    // Сокращённые ключи (stat_t вместо state_topic) HA понимает
    // наравне с полными, а на ESP экономят сотни байт на сообщение.
    String topic = String(MQTT_DISCOVERY_PREFIX) + "/" + e.component + "/"
                 + _id + "/" + e.object + "/config";

    String p = "{";
    p += F("\"name\":\"");
    p += String(e.name);
    p += F("\",");
    p += F("\"uniq_id\":\"");
    p += String(_id);
    p += F("_");
    p += e.object;
    p += F("\",");
    p += F("\"avty_t\":\"");
    p += String(_base);
    p += F("/status\",");

    if (strcmp(e.component, "light") == 0) {
      p += F("\"stat_t\":\"");
      p += String(_base);
      p += F("/light\",");
    } else if (e.valueKey) {
      p += F("\"stat_t\":\"");
      p += String(_base);
      p += F("/state\",");
      p += F("\"val_tpl\":\"{{ value_json.");
      p += String(e.valueKey);
      p += F(" }}\",");
    }
    if (e.command) {
      p += F("\"cmd_t\":\"");
      p += String(_base);
      p += F("/cmd/");
      p += e.command;
      p += F("\",");
    }
    if (e.deviceClass) p += "\"dev_cla\":\"" + String(e.deviceClass) + "\",";
    if (e.stateClass)  p += "\"stat_cla\":\"" + String(e.stateClass) + "\",";
    if (e.unit)        p += "\"unit_of_meas\":\"" + String(e.unit) + "\",";
    if (e.category)    p += "\"ent_cat\":\"" + String(e.category) + "\",";
    if (e.extra)       p += String(e.extra) + ",";

    // Общий блок устройства — по нему HA группирует сущности в одну
    // карточку
    p += F("\"dev\":{\"ids\":[\"");
    p += String(_id);
    p += F("\"],");
    p += F("\"name\":\"");
    p += String(_friendly);
    p += F("\",");
    p += F("\"mdl\":\"Wemos D1 mini\",\"mf\":\"DIY\",");
    p += F("\"sw\":\"");
    p += String(_swVersion);
    p += F("\"}}");

    // retain: HA восстановит сущности после своего перезапуска, не
    // дожидаясь следующей загрузки светильника
    _mqtt.publish(topic.c_str(), 0, true, p.c_str());
  }

  void onConnected() {
    _connected = true;
    _reconnects++;
    Log.printf("[MQTT] Подключено к %s:%u\n",
               _config->data.mqttHost, _config->data.mqttPort);

    String statusTopic = String(_base) + "/status";
    _mqtt.publish(statusTopic.c_str(), 1, true, "online");

    String cmdTopic = String(_base) + "/cmd/+";
    _mqtt.subscribe(cmdTopic.c_str(), 1);

    _discoveryStep = 0;
    _lastDiscoveryMs = millis();
    _lastPublish = 0;
  }

public:
  MqttClient()
    : _config(nullptr), _cb(nullptr), _enabled(false), _connected(false),
      _lastAttempt(0), _lastPublish(0), _discoveryStep(-1),
      _lastDiscoveryMs(0), _publishCount(0), _reconnects(0), _lastError(-1) {
    _id[0] = _base[0] = _friendly[0] = _swVersion[0] = '\0';
  }

  void begin(ConfigStore *config, const uint8_t *mac,
             const char *friendlyName, const char *swVersion,
             CommandCallback cb) {
    _config = config;
    _cb = cb;
    _instance = this;

    snprintf(_id, sizeof(_id), "snl%02X%02X%02X", mac[3], mac[4], mac[5]);
    snprintf(_base, sizeof(_base), "smartnightlight/%02X%02X%02X",
             mac[3], mac[4], mac[5]);
    strlcpy(_friendly, friendlyName, sizeof(_friendly));
    strlcpy(_swVersion, swVersion, sizeof(_swVersion));

    _mqtt.onConnect([](bool sessionPresent) {
      if (_instance) _instance->onConnected();
    });

    _mqtt.onDisconnect([](AsyncMqttClientDisconnectReason reason) {
      if (!_instance) return;
      _instance->_connected = false;
      _instance->_discoveryStep = -1;
      _instance->_lastError = (int8_t)reason;
      Log.printf("[MQTT] Отключено: %s (%d)\n",
                 errorText((int8_t)reason), (int)reason);
    });

    _mqtt.onMessage([](char *topic, char *payload,
                       AsyncMqttClientMessageProperties props,
                       size_t len, size_t index, size_t total) {
      if (!_instance) return;
      // payload не заканчивается нулём — берём ровно len байт
      String value;
      value.concat(payload, len);
      String t(topic);
      int slash = t.lastIndexOf('/');
      String cmd = (slash >= 0) ? t.substring(slash + 1) : t;
      Log.printf("[MQTT] Команда '%s' = '%s'\n", cmd.c_str(), value.c_str());
      if (_instance->_cb) _instance->_cb(cmd, value);
    });

    applyConfig();
  }

  // Перечитать настройки. Вызывается при старте и после сохранения,
  // чтобы не требовать перезагрузки.
  void applyConfig() {
    if (!_config) return;
    _enabled = _config->data.mqttEnabled && strlen(_config->data.mqttHost) > 0;

    if (!_enabled) {
      if (_connected) _mqtt.disconnect();
      Log.println(F("[MQTT] Выключен"));
      return;
    }

    _mqtt.setServer(_config->data.mqttHost, _config->data.mqttPort);
    if (strlen(_config->data.mqttUser) > 0) {
      _mqtt.setCredentials(_config->data.mqttUser, _config->data.mqttPass);
    }
    _mqtt.setClientId(_id);

    // Last Will: если связь оборвётся, брокер сам объявит устройство
    // недоступным — HA узнает сразу, а не по таймауту
    static String willTopic;
    willTopic = String(_base) + "/status";
    _mqtt.setWill(willTopic.c_str(), 1, true, "offline");

    _lastAttempt = 0;
    Log.printf("[MQTT] Включён, брокер %s:%u\n",
               _config->data.mqttHost, _config->data.mqttPort);
  }

  void handle(const Payload &p) {
    if (!_enabled || WiFi.status() != WL_CONNECTED) return;

    uint32_t now = millis();

    if (!_connected) {
      if (_lastAttempt == 0 || now - _lastAttempt > MQTT_RECONNECT_MS) {
        _lastAttempt = now;
        Log.println(F("[MQTT] Подключаюсь..."));
        _mqtt.connect();      // не блокирует: ESPAsyncTCP
      }
      return;
    }

    if (_discoveryStep >= 0) {
      if (now - _lastDiscoveryMs >= MQTT_DISCOVERY_STEP_MS) {
        int count;
        const EntityDef *defs = entities(count);
        publishDiscoveryEntity(defs[_discoveryStep]);
        _lastDiscoveryMs = now;
        _discoveryStep++;
        if (_discoveryStep >= count) {
          _discoveryStep = -1;
          Log.printf("[MQTT] Автодискавери опубликовано: %d сущностей\n", count);
        }
      }
      return;   // состояние подождёт, пока не разошлём описания
    }

    if (_lastPublish == 0 || now - _lastPublish >= intervalMs()) {
      publishState(p);
      _lastPublish = now;
    }
  }

  void publishState(const Payload &p) {
    if (!_connected) return;

    {
      JsonDocument doc;
      doc["brightness"] = p.brightness;
      doc["mode"] = p.mode;

      JsonObject s = doc["sound"].to<JsonObject>();
      s["level"] = p.soundLevel;
      s["peak"] = p.soundPeak;
      s["triggers"] = p.soundTriggers;
      s["triggers_out"] = p.soundTriggersOut;
      s["active"] = p.soundActive ? "ON" : "OFF";

      JsonObject m = doc["modes"].to<JsonObject>();
      m["sleep"] = p.sleepEnabled ? "ON" : "OFF";
      m["sunrise"] = p.sunriseEnabled ? "ON" : "OFF";
      m["sound"] = p.soundEnabled ? "ON" : "OFF";

      JsonObject c = doc["cfg"].to<JsonObject>();
      c["manual_brightness"] = p.manualBrightness;
      c["sound_threshold"] = _config->data.soundThreshold;
      c["sound_brightness"] = _config->data.soundBrightness;

      JsonObject sys = doc["sys"].to<JsonObject>();
      sys["rssi"] = p.rssi;
      sys["uptime"] = p.uptimeSec;
      sys["heap"] = p.freeHeap;
      sys["time_valid"] = p.timeValid ? "ON" : "OFF";

      String out;
      serializeJson(doc, out);
      String topic = String(_base) + "/state";
      _mqtt.publish(topic.c_str(), 0, false, out.c_str());
    }

    // Лампа отдельным сообщением в формате, который ждёт JSON-схема HA.
    // Документ выше к этому моменту уже освобождён: два больших JSON
    // одновременно на этой куче держать незачем.
    {
      JsonDocument doc;
      doc["state"] = p.manualOn ? "ON" : "OFF";
      doc["brightness"] = p.manualBrightness;
      String out;
      serializeJson(doc, out);
      String topic = String(_base) + "/light";
      _mqtt.publish(topic.c_str(), 0, true, out.c_str());
    }

    _publishCount++;
  }

  // Снять описания сущностей с брокера. Пустой retained-payload —
  // штатный способ убрать сущность из Home Assistant.
  void clearDiscovery() {
    if (!_connected) return;
    int count;
    const EntityDef *defs = entities(count);
    for (int i = 0; i < count; i++) {
      String topic = String(MQTT_DISCOVERY_PREFIX) + "/" + defs[i].component
                   + "/" + _id + "/" + defs[i].object + "/config";
      _mqtt.publish(topic.c_str(), 0, true, "");
    }
    Log.println(F("[MQTT] Автодискавери снято"));
  }

  // Без расшифровки причина отключения — это цифра в логе, а в
  // интерфейсе просто «нет связи». Чаще всего дело в логине, и знать
  // это надо сразу: брокер с паролем молча отвергает подключение.
  static const char *errorText(int8_t reason) {
    switch (reason) {
      case -1: return "";
      case 0:  return "нет TCP-соединения";
      case 1:  return "брокер не принял версию протокола";
      case 2:  return "брокер отверг идентификатор клиента";
      case 3:  return "брокер недоступен";
      case 4:  return "неверный формат логина или пароля";
      case 5:  return "не авторизован: нужен логин и пароль";
      case 6:  return "не хватило памяти";
      case 7:  return "не совпал отпечаток TLS";
      default: return "неизвестная причина";
    }
  }

  int8_t lastError() const { return _lastError; }
  const char *lastErrorText() const { return errorText(_lastError); }

  bool isEnabled() const { return _enabled; }
  bool isConnected() const { return _connected; }
  uint32_t publishCount() const { return _publishCount; }
  uint32_t reconnects() const { return _reconnects; }
  const char *baseTopic() const { return _base; }
};

MqttClient *MqttClient::_instance = nullptr;

#endif
