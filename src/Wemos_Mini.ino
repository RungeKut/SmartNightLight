/******************************************************************
 * SmartNightLight — умный ночник на Wemos D1 mini (ESP8266)
 *
 * Пароль от домашней сети в прошивке больше не лежит. На чистой
 * плате устройство поднимает открытую точку доступа
 * SmartNightLight-XXXXXX на 192.168.0.1, где в веб-интерфейсе
 * вводятся SSID и пароль. Дальше они живут в EEPROM.
 ******************************************************************/

#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <LittleFS.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <NTPClient.h>
#include <time.h>

#include "Log.h"
#include "NightLight.h"
#include "ConfigStore.h"
#include "WsRxBuffer.h"
#include "SoundSensor.h"
#include "MqttClient.h"
#include "FailsafeOTA.h"

// ==================== Железо ====================
// GPIO0 (D3). Пин участвует в выборе режима загрузки, поэтому в
// момент сброса он должен быть подтянут вверх — драйвер светодиода
// не должен сажать его к земле.
#define LED_PIN 0
#define PWM_FREQ_HZ 1000
#define PWM_BITS 8

// Префикс имени устройства и SSID точки доступа
#define AP_SSID_PREFIX "SmartNightLight"
// Пустой пароль = открытая сеть. Точка доступа поднимается только
// когда домашняя сеть недоступна, и нужна ровно для того, чтобы
// ввести пароль от неё.
#define AP_PASS ""

// Период вспышек "пора спать"
#define BLINK_PERIOD_MS 200
// Настройки, изменённые ползунком, пишем в EEPROM не сразу
#define SAVE_DEFER_MS 30000
// Период рассылки телеметрии подключённым вкладкам
#define TELEMETRY_MS 1000

// ==================== Глобальное состояние ====================
ConfigStore config;
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
WsRxBuffer wsRx;
SoundSensor sound;
MqttClient mqtt;
FailsafeOTA failsafe;

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "europe.pool.ntp.org", 0, 60000);

char deviceName[32];
char defaultDeviceName[32];
char apSSID[32];
uint8_t deviceMac[6];

bool wifiConnected = false;
bool apModeActive = false;
uint32_t lastWiFiRetry = 0;

// Время. NTPClient продолжает считать по millis() от последней
// синхронизации, поэтому часы идут и при пропавшей сети.
time_t epochTime = 0;
bool timeValid = false;
uint16_t nowHhmm = 0;
uint16_t nowYear = 1970;
uint8_t nowMonth = 1, nowDay = 1, nowSecond = 0;

// Ручное включение — состояние на сеанс, в EEPROM не сохраняется:
// после перезагрузки светильник должен вернуться к расписанию.
bool manualOn = false;

// Отклик на шум. Момент последнего превышения порога хранится здесь,
// а не в NightLight.h: логика не должна знать про millis().
uint32_t soundTriggerMs = 0;
bool soundActive = false;

uint8_t currentBrightness = 0;
nl::Mode currentMode = nl::MODE_OFF;
uint16_t lastWritten = 0xFFFF;   // заведомо отличается от любой яркости

bool configDirty = false;
uint32_t configDirtyAt = 0;
bool needsRestart = false;
uint32_t restartAtMs = 0;

uint32_t lastTelemetry = 0;
uint32_t bootMillis = 0;

// Снимок состояния отправляется из loop(), а не из колбэка WS:
// колбэк выполняется в контексте сетевого стека, и отправка
// большого JSON прямо оттуда роняет плату.
#define SNAPSHOT_SLOTS 4
#define SNAPSHOT_STEP_MS 60
struct SnapshotReq { uint32_t clientId; uint8_t step; };
SnapshotReq snapshots[SNAPSHOT_SLOTS];
uint32_t snapshotLastMs = 0;
uint32_t snapshotDropped = 0;

// ==================== Прототипы ====================
void wsSendJson(AsyncWebSocketClient *client, const JsonDocument &doc);
void handleWsMessage(AsyncWebSocketClient *client, const String &msg);
void sendSystemState(AsyncWebSocketClient *client);
void sendConfigState(AsyncWebSocketClient *client);
void requestSnapshot(uint32_t clientId);
void handleSnapshots(uint32_t now);
void broadcastTelemetry();
void setupHttpRoutes();
void fillSystemState(JsonDocument &doc);
bool connectToWiFi();
void startAPMode();
void WiFiupd();
void updateSound(uint32_t now);
void applyNightLight();
void markConfigDirty();
void onMqttCommand(const String &cmd, const String &value);
MqttClient::Payload buildMqttPayload();

// ==================== Светодиод ====================
void setBrightness(uint8_t value) {
  if (lastWritten == value) return;
  analogWrite(LED_PIN, value);
  lastWritten = value;
}

// ==================== Имя устройства ====================
// Латиница, цифры и дефис — имя уходит в hostname, mDNS и SSID.
// Кириллическое имя отсеется целиком, и включится имя по MAC.
//
// Размер источника обязателен: поле в EEPROM может не содержать
// нуля вовсе, и цикл по src[i] ушёл бы читать соседнюю память.
void sanitizeDeviceName(const char *src, size_t srcSize, char *dst, size_t dstSize) {
  size_t j = 0;
  for (size_t i = 0; i < srcSize && src[i] != '\0' && j + 1 < dstSize; i++) {
    char c = src[i];
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
      dst[j++] = c;
    } else if (c == '-' || c == ' ' || c == '_' || c == '.') {
      if (j > 0 && dst[j - 1] != '-') dst[j++] = '-';
    }
  }
  while (j > 0 && dst[j - 1] == '-') j--;
  dst[j] = '\0';
}

void applyDeviceName() {
  char clean[sizeof(deviceName)];
  sanitizeDeviceName(config.data.deviceName, sizeof(config.data.deviceName),
                     clean, sizeof(clean));
  if (strlen(clean) > 0) {
    strlcpy(deviceName, clean, sizeof(deviceName));
  } else {
    strlcpy(deviceName, defaultDeviceName, sizeof(deviceName));
  }
  strlcpy(apSSID, deviceName, sizeof(apSSID));
}

// ==================== Время ====================
void updateLocalTime() {
  if (wifiConnected) timeClient.update();

  epochTime = timeClient.getEpochTime();
  // До первой синхронизации NTPClient отдаёт смещение плюс аптайм.
  // Любая дата раньше 2020 года означает, что времени у нас нет.
  timeValid = (epochTime > 1600000000);

  struct tm *ptm = gmtime(&epochTime);
  if (ptm == nullptr) return;
  nowHhmm = (uint16_t)(ptm->tm_hour * 100 + ptm->tm_min);
  nowSecond = (uint8_t)ptm->tm_sec;
  nowYear = (uint16_t)(ptm->tm_year + 1900);
  nowMonth = (uint8_t)(ptm->tm_mon + 1);
  nowDay = (uint8_t)ptm->tm_mday;
}

// ==================== Микрофон ====================
// Детектор работает всё ночное окно, даже когда светильник занят
// другим режимом: срабатывания нужны для графика ночной активности,
// а зажигать свет или нет — решает уже nl::compute().
void updateSound(uint32_t now) {
  if (!sound.tick(now)) return;

  nl::Settings s = config.toSettings();
  bool inWindow = timeValid
               && nl::insideWindow(s.soundStart, s.soundEnd, nowHhmm);

  if ((config.data.flags & nl::FLAG_SOUND) && inWindow
      && sound.exceeded(config.data.soundThreshold)) {
    // Новый шум во время отклика продлевает его с начала: ребёнок,
    // который ходит по комнате, не должен остаться в темноте на
    // середине затухания.
    if (!soundActive) sound.countTrigger();   // считаем события, а не измерения
    soundTriggerMs = now;
    soundActive = true;
  }

  if (soundActive && (now - soundTriggerMs) / 1000 > nl::soundTotalSec(s)) {
    soundActive = false;
  }
}

// ==================== Логика светильника ====================
void applyNightLight() {
  nl::Settings s = config.toSettings();
  bool blinkOn = ((millis() / BLINK_PERIOD_MS) % 2) == 0;

  nl::SoundEvent ev;
  ev.active = soundActive;
  ev.secSince = soundActive ? (millis() - soundTriggerMs) / 1000 : 0;

  nl::Result r = nl::compute(s, nowHhmm, manualOn, blinkOn, ev);

  // Без синхронизированного времени расписание работать не может:
  // gmtime(0) дал бы 00:00 01.01.1970, и интервалы срабатывали бы
  // наугад. В таком состоянии доступно только ручное включение.
  if (!timeValid && !manualOn) {
    r.brightness = 0;
    r.mode = nl::MODE_OFF;
  }

  currentBrightness = r.brightness;
  currentMode = r.mode;
  setBrightness(r.brightness);
}

// ==================== Отложенная запись настроек ====================
// Ползунок яркости шлёт значение на каждое движение. Писать EEPROM
// на каждое из них — верный способ израсходовать ресурс flash.
void markConfigDirty() {
  configDirty = true;
  configDirtyAt = millis();
}

void flushConfig() {
  if (!configDirty) return;
  if (millis() - configDirtyAt < SAVE_DEFER_MS) return;
  config.save();
  configDirty = false;
}

// ==================== SETUP ====================
void setup() {
  Serial.begin(115200);
  bootMillis = millis();

  pinMode(LED_PIN, OUTPUT);
  analogWriteFreq(PWM_FREQ_HZ);
  analogWriteResolution(PWM_BITS);   // иначе диапазон PWM 0..1023
  setBrightness(0);

  WiFi.macAddress(deviceMac);
  snprintf(defaultDeviceName, sizeof(defaultDeviceName), "%s-%02X%02X%02X",
           AP_SSID_PREFIX, deviceMac[3], deviceMac[4], deviceMac[5]);

  config.begin();
  applyDeviceName();

  Log.printf("\n\n=== %s ===\n", deviceName);
  Log.printf("[Boot] Причина сброса: %s\n", ESP.getResetReason().c_str());

  failsafe.begin();
  sound.begin();

  timeClient.begin();
  timeClient.setTimeOffset(config.data.tzOffsetMinutes * 60);

  if (!connectToWiFi()) startAPMode();

  if (!LittleFS.begin()) {
    Log.println(F("[FS] LittleFS не смонтирована — залейте фронтенд: pio run -t uploadfs"));
  }

  ArduinoOTA.setHostname(deviceName);
  ArduinoOTA.onStart([]() {
    // Образ файловой системы пишется поверх той самой LittleFS, из
    // которой сейчас отдаётся страница. Не размонтировать её — значит
    // писать под работающей ФС и получить мусор вместо фронтенда.
    if (ArduinoOTA.getCommand() == U_FS) {
      Log.println(F("[OTA] Обновление файловой системы, размонтирую LittleFS"));
      LittleFS.end();
    } else {
      Log.println(F("[OTA] Обновление прошивки"));
    }
    // Гасим светильник: во время записи flash цикл не крутится, и
    // яркость всё равно застыла бы на текущей до перезагрузки.
    setBrightness(0);
  });
  ArduinoOTA.onEnd([]() {
    Log.println(F("[OTA] Готово, перезагрузка"));
    // Флаг в RTC: новая прошивка при старте узнает, что её надо
    // подтвердить. Только для образа прошивки — обновление
    // файловой системы код не меняет и подтверждения не требует.
    if (ArduinoOTA.getCommand() == U_FLASH) failsafe.updateFirmware();
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Log.printf("[OTA] Ошибка %u\n", error);
    // Файловую систему после неудачного обновления надо вернуть, иначе
    // до перезагрузки страница отдаваться не будет.
    LittleFS.begin();
  });
  ArduinoOTA.begin();

  ws.onEvent([](AsyncWebSocket *server, AsyncWebSocketClient *client,
                AwsEventType type, void *arg, uint8_t *data, size_t len) {
    if (type == WS_EVT_CONNECT) {
      Log.printf("[WS] Клиент #%u подключился\n", client->id());
      // Потеря кадра телеметрии лучше обрыва: при разрыве браузер
      // перезагружает страницу поверх незаконченного ввода.
      client->setCloseClientOnQueueFull(false);
      // Браузер при обновлении вкладки бросает сокет, не закрывая
      // его. Без пингов такие призраки копятся в списке клиентов.
      client->keepAlivePeriod(10);
      requestSnapshot(client->id());
    } else if (type == WS_EVT_DISCONNECT) {
      Log.printf("[WS] Клиент #%u отключился\n", client->id());
    } else if (type == WS_EVT_DATA) {
      AwsFrameInfo *info = (AwsFrameInfo*)arg;
      if (info->opcode != WS_TEXT && info->opcode != WS_CONTINUATION) return;
      // Сообщение длиннее TCP-сегмента (~536 байт) приезжает
      // несколькими вызовами, и saveConfig в один не помещается.
      bool complete = wsRx.feed(client->id(), (uint8_t)info->opcode,
                                info->final, (uint32_t)info->index,
                                (uint32_t)len, (uint32_t)info->len, data);
      if (complete) {
        String payload;
        payload.concat(wsRx.message(), wsRx.length());
        wsRx.reset();
        handleWsMessage(client, payload);
      }
    }
  });
  server.addHandler(&ws);
  setupHttpRoutes();
  server.begin();

  if (MDNS.begin(deviceName)) {
    MDNS.addService("http", "tcp", 80);
    Log.printf("[mDNS] http://%s.local\n", deviceName);
  }

  // Версия прошивки = дата сборки, её видно в карточке устройства HA
  mqtt.begin(&config, deviceMac, deviceName, __DATE__, onMqttCommand);

  Log.println(F("[Boot] Готово"));
}

// ==================== LOOP ====================
void loop() {
  uint32_t now = millis();

  ArduinoOTA.handle();
  MDNS.update();
  // Без этого отключившийся клиент остаётся в списке навсегда:
  // события WS_EVT_DISCONNECT библиотека не выдаёт сама, а
  // availableForWriteAll() требует свободной очереди у ВСЕХ
  // клиентов. Один призрак — и телеметрия замолкает для всех.
  ws.cleanupClients(2);
  WiFiupd();
  updateLocalTime();
  updateSound(now);
  applyNightLight();
  flushConfig();
  handleSnapshots(now);
  failsafe.handle();
  mqtt.handle(buildMqttPayload());

  if (now - lastTelemetry >= TELEMETRY_MS) {
    lastTelemetry = now;
    broadcastTelemetry();
  }

  if (needsRestart && restartAtMs == 0) {
    // Печатаем здесь, а не перед самым ESP.restart(): вывод
    // асинхронный, и строка за миллисекунду до сброса не доедет.
    restartAtMs = now + 500;
    Log.println(F("[System] Перезагрузка запланирована"));
  }
  if (restartAtMs != 0 && (int32_t)(now - restartAtMs) >= 0) {
    if (configDirty) { config.save(); configDirty = false; }
    ESP.restart();
  }

  delay(10);
}

// ==================== WiFi ====================
bool connectToWiFi() {
  if (strlen(config.data.wifiSSID) == 0) {
    Log.println(F("[WiFi] SSID не задан, поднимаю точку доступа"));
    return false;
  }
  Log.printf("[WiFi] Подключаюсь к '%s'...\n", config.data.wifiSSID);
  WiFi.persistent(false);
  WiFi.disconnect(true);
  delay(100);
  WiFi.mode(WIFI_OFF);
  delay(100);
  WiFi.mode(WIFI_STA);
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  // Имя обязательно до begin(), иначе роутер увидит имя по умолчанию
  WiFi.hostname(deviceName);
  WiFi.begin(config.data.wifiSSID, config.data.wifiPass);

  for (int i = 0; i < 60 && WiFi.status() != WL_CONNECTED; i++) {
    delay(500);
    Log.print(".");
  }
  Log.println();

  if (WiFi.status() == WL_CONNECTED) {
    Log.printf("[WiFi] Подключено, IP: %s\n", WiFi.localIP().toString().c_str());
    wifiConnected = true;
    apModeActive = false;
    return true;
  }
  Log.printf("[WiFi] Не удалось (status: %d)\n", WiFi.status());
  wifiConnected = false;
  return false;
}

void startAPMode() {
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(IPAddress(192, 168, 0, 1),
                    IPAddress(192, 168, 0, 1),
                    IPAddress(255, 255, 255, 0));
  WiFi.softAP(apSSID, AP_PASS);
  Log.printf("[WiFi] Точка доступа: %s (192.168.0.1)\n", apSSID);
  apModeActive = true;
  wifiConnected = false;
  lastWiFiRetry = millis();
}

// Переподключение без блокировки loop(): и портал, и сам светильник
// должны работать, пока роутер недоступен.
void WiFiupd() {
  uint32_t now = millis();

  if (apModeActive) {
    // Раз в 10 минут пробуем вернуться в домашнюю сеть: роутер мог
    // просто перезагружаться, когда мы поднимали точку доступа.
    if (strlen(config.data.wifiSSID) == 0) return;
    if (now - lastWiFiRetry < 600000) return;
    lastWiFiRetry = now;
    Log.println(F("[WiFi] Пробую вернуться в домашнюю сеть..."));
    WiFi.softAPdisconnect(true);
    delay(100);
    if (connectToWiFi()) return;
    startAPMode();
    return;
  }

  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    return;
  }

  wifiConnected = false;
  if (now - lastWiFiRetry < 30000) return;
  lastWiFiRetry = now;
  Log.println(F("[WiFi] Связь потеряна, переподключаюсь..."));
  WiFi.hostname(deviceName);   // после WIFI_OFF имя может не сохраниться
  WiFi.begin(config.data.wifiSSID, config.data.wifiPass);
}

// ==================== WebSocket: отправка ====================
void wsSendJson(AsyncWebSocketClient *client, const JsonDocument &doc) {
  String json;
  serializeJson(doc, json);
  if (client && client->status() == WS_CONNECTED) client->text(json);
}

void fillSystemState(JsonDocument &doc) {
  doc["device"] = deviceName;
  doc["ap_mode"] = apModeActive;
  doc["ap_ssid"] = apSSID;
  doc["wifi"] = wifiConnected ? "connected" : "disconnected";
  doc["wifi_rssi"] = wifiConnected ? WiFi.RSSI() : 0;
  doc["ip"] = apModeActive ? WiFi.softAPIP().toString() : WiFi.localIP().toString();

  doc["time_valid"] = timeValid;
  doc["time_hhmm"] = nowHhmm;
  doc["time_sec"] = nowSecond;
  doc["date_year"] = nowYear;
  doc["date_month"] = nowMonth;
  doc["date_day"] = nowDay;

  doc["manual_on"] = manualOn;
  doc["brightness"] = currentBrightness;
  doc["mode"] = nl::modeName(currentMode);

  // Уровень и пик нужны, чтобы подобрать порог: в интерфейсе видно,
  // что даёт тишина, а что — шаги по комнате.
  doc["sound_level"] = sound.level();
  doc["sound_peak"] = sound.peak();
  doc["sound_triggers"] = sound.triggers();
  doc["sound_present"] = sound.present();
  doc["sound_active"] = soundActive;

  doc["mqtt_enabled"] = mqtt.isEnabled();
  doc["mqtt_connected"] = mqtt.isConnected();
  doc["mqtt_error"] = mqtt.lastErrorText();

  doc["ota_pending"] = failsafe.isPending();
  doc["ota_remaining"] = failsafe.remainingSec();

  doc["uptime"] = (millis() - bootMillis) / 1000;
  doc["heap"] = ESP.getFreeHeap();
  doc["ws_clients"] = ws.count();
  doc["ws_drops"] = wsRx.drops();
  doc["config_pending"] = configDirty;
}

// ==================== MQTT ====================
MqttClient::Payload buildMqttPayload() {
  MqttClient::Payload p;
  p.brightness = currentBrightness;
  p.mode = nl::modeName(currentMode);
  p.manualOn = manualOn;
  p.manualBrightness = config.data.manualBrightness;

  p.soundLevel = sound.level();
  p.soundPeak = sound.peak();
  p.soundTriggers = sound.triggers();
  p.soundActive = soundActive;

  p.sleepEnabled = (config.data.flags & nl::FLAG_SLEEP) != 0;
  p.sunriseEnabled = (config.data.flags & nl::FLAG_SUNRISE) != 0;
  p.soundEnabled = (config.data.flags & nl::FLAG_SOUND) != 0;
  p.timeValid = timeValid;

  p.rssi = wifiConnected ? WiFi.RSSI() : 0;
  p.uptimeSec = (millis() - bootMillis) / 1000;
  p.freeHeap = ESP.getFreeHeap();
  return p;
}

// Переключить бит режима и запомнить, что настройки надо сохранить
static void setFlag(uint8_t bit, bool on) {
  if (on) config.data.flags |= bit;
  else config.data.flags &= ~bit;
  markConfigDirty();
}

// Команды из Home Assistant. Приходят в контексте сетевого стека,
// поэтому здесь только меняем состояние — работу делает loop().
void onMqttCommand(const String &cmd, const String &value) {
  if (cmd == "light") {
    // JSON-схема HA: {"state":"ON","brightness":128}
    JsonDocument doc;
    if (deserializeJson(doc, value)) {
      Log.println(F("[MQTT] Не разобрал команду лампы"));
      return;
    }
    const char *state = doc["state"] | "";
    if (doc["brightness"].is<int>()) {
      int b = doc["brightness"].as<int>();
      config.data.manualBrightness = (uint8_t)constrain(b, 0, 255);
      markConfigDirty();
    }
    if (strcmp(state, "ON") == 0) manualOn = true;
    else if (strcmp(state, "OFF") == 0) manualOn = false;
    applyNightLight();
    mqtt.publishState(buildMqttPayload());   // HA ждёт подтверждения сразу
  }
  else if (cmd == "mode_sleep")   setFlag(nl::FLAG_SLEEP, value == "ON");
  else if (cmd == "mode_sunrise") setFlag(nl::FLAG_SUNRISE, value == "ON");
  else if (cmd == "mode_sound")   setFlag(nl::FLAG_SOUND, value == "ON");
  else if (cmd == "sound_threshold") {
    config.data.soundThreshold = (uint16_t)constrain(value.toInt(), 0, 1023);
    markConfigDirty();
  }
  else if (cmd == "sound_brightness") {
    config.data.soundBrightness = (uint8_t)constrain(value.toInt(), 0, 255);
    markConfigDirty();
  }
  else if (cmd == "restart") needsRestart = true;
  else if (cmd == "confirm_ota") failsafe.confirm();
}

void sendSystemState(AsyncWebSocketClient *client) {
  JsonDocument doc;
  doc["type"] = "fullState";
  fillSystemState(doc);
  wsSendJson(client, doc);
}

void sendConfigState(AsyncWebSocketClient *client) {
  JsonDocument doc;
  doc["type"] = "configState";
  JsonObject cfg = doc["config"].to<JsonObject>();
  cfg["wifiSSID"] = config.data.wifiSSID;
  cfg["deviceName"] = config.data.deviceName;
  cfg["defaultDeviceName"] = defaultDeviceName;
  cfg["flags"] = config.data.flags;
  cfg["manualBrightness"] = config.data.manualBrightness;
  cfg["sleepTime"] = config.data.sleepTime;
  cfg["sleepLength"] = config.data.sleepLength;
  cfg["signalBrightness"] = config.data.signalBrightness;
  cfg["sunriseTime"] = config.data.sunriseTime;
  cfg["sunriseLength"] = config.data.sunriseLength;
  cfg["sunLength"] = config.data.sunLength;
  cfg["sunriseBrightness"] = config.data.sunriseBrightness;
  cfg["tzOffsetMinutes"] = config.data.tzOffsetMinutes;
  cfg["signalMinutes"] = nl::SIGNAL_MINUTES;

  cfg["soundStart"] = config.data.soundStart;
  cfg["soundEnd"] = config.data.soundEnd;
  cfg["soundThreshold"] = config.data.soundThreshold;
  cfg["soundBrightness"] = config.data.soundBrightness;
  cfg["soundFadeInSec"] = config.data.soundFadeInSec;
  cfg["soundHoldSec"] = config.data.soundHoldSec;
  cfg["soundFadeOutSec"] = config.data.soundFadeOutSec;

  cfg["mqttEnabled"] = config.data.mqttEnabled;
  cfg["mqttHost"] = config.data.mqttHost;
  cfg["mqttPort"] = config.data.mqttPort;
  cfg["mqttUser"] = config.data.mqttUser;
  cfg["mqttIntervalSec"] = config.data.mqttIntervalSec;

  // Пароли (wifiPass, mqttPass) клиенту намеренно не отдаём
  wsSendJson(client, doc);
}

// Телеметрия раз в секунду. Очередь клиента на ESP8266 — 8
// сообщений; при переполнении библиотека по умолчанию рвёт
// соединение. Пропущенный кадр дешевле обрыва: следующий придёт
// через секунду, а форма настроек не сбросится.
void broadcastTelemetry() {
  if (ws.count() == 0) return;
  if (!ws.availableForWriteAll()) return;
  JsonDocument doc;
  doc["type"] = "telemetry";
  fillSystemState(doc);
  String json;
  serializeJson(doc, json);
  ws.textAll(json);
}

// Поставить снимок в очередь. Повторный запрос от того же клиента
// перезапускает отправку с начала, а не заводит второй слот.
void requestSnapshot(uint32_t clientId) {
  for (int i = 0; i < SNAPSHOT_SLOTS; i++) {
    if (snapshots[i].step != 0 && snapshots[i].clientId == clientId) {
      snapshots[i].step = 1;
      return;
    }
  }
  for (int i = 0; i < SNAPSHOT_SLOTS; i++) {
    if (snapshots[i].step == 0) {
      snapshots[i].clientId = clientId;
      snapshots[i].step = 1;
      return;
    }
  }
  snapshotDropped++;
}

// Один шаг за проход loop(). Клиент мог отвалиться, пока снимок
// ждал очереди, — тогда слот просто освобождается.
void handleSnapshots(uint32_t now) {
  if (now - snapshotLastMs < SNAPSHOT_STEP_MS) return;

  for (int i = 0; i < SNAPSHOT_SLOTS; i++) {
    if (snapshots[i].step == 0) continue;

    AsyncWebSocketClient *c = ws.client(snapshots[i].clientId);
    if (c == nullptr || c->status() != WS_CONNECTED) {
      snapshots[i].step = 0;
      continue;
    }
    if (!ws.availableForWrite(snapshots[i].clientId)) return;

    switch (snapshots[i].step) {
      case 1: sendSystemState(c); break;
      case 2: sendConfigState(c); break;
    }
    snapshots[i].step++;
    if (snapshots[i].step > 2) snapshots[i].step = 0;
    snapshotLastMs = now;
    return;   // не больше одного сообщения за проход
  }
}

// ==================== WebSocket: приём ====================
// Пустое значение пароля = оставить прежний. Сервер не отдаёт
// пароль клиенту, поэтому поле в форме всегда пустое, и без этого
// правила сохранение любой настройки стирало бы пароль от WiFi.
static void keepOrSet(char *dst, JsonVariant src, size_t size) {
  const char *v = src.is<const char*>() ? src.as<const char*>() : nullptr;
  if (v == nullptr || v[0] == '\0') return;
  strlcpy(dst, v, size);
}

void handleWsMessage(AsyncWebSocketClient *client, const String &msg) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, msg);
  if (err) {
    Log.printf("[WS] Ошибка разбора: %s\n", err.c_str());
    return;
  }

  const char *type = doc["type"];
  if (type == nullptr) {
    Log.println(F("[WS] Сообщение без поля type, игнорирую"));
    return;
  }

  if (strcmp(type, "getFullState") == 0) {
    // Обработчик вызывается из колбэка сетевого стека — откладываем
    requestSnapshot(client->id());
  }
  else if (strcmp(type, "setManual") == 0) {
    // Ручное управление с вкладки Dashboard: применяется сразу,
    // без сохранения и перезагрузки.
    if (doc["on"].is<bool>()) manualOn = doc["on"].as<bool>();
    if (doc["brightness"].is<int>()) {
      int b = doc["brightness"].as<int>();
      if (b < 0) b = 0;
      if (b > 255) b = 255;
      config.data.manualBrightness = (uint8_t)b;
      markConfigDirty();
    }
    applyNightLight();
  }
  else if (strcmp(type, "saveConfig") == 0) {
    JsonObject cfg = doc["config"];
    if (cfg.isNull()) {
      // Без объекта config все поля ушли бы в EEPROM пустыми
      Log.println(F("[WS] saveConfig без объекта config, игнорирую"));
      JsonDocument resp;
      resp["type"] = "saveConfigResult";
      resp["success"] = false;
      resp["message"] = "Некорректный запрос";
      wsSendJson(client, resp);
      return;
    }

    // Смена сети или имени требует перезагрузки, остальное
    // применяется на лету — ночник не должен гаснуть из-за того,
    // что подвинули ползунок.
    char prevSSID[sizeof(config.data.wifiSSID)];
    char prevPass[sizeof(config.data.wifiPass)];
    char prevName[sizeof(config.data.deviceName)];
    strlcpy(prevSSID, config.data.wifiSSID, sizeof(prevSSID));
    strlcpy(prevPass, config.data.wifiPass, sizeof(prevPass));
    strlcpy(prevName, config.data.deviceName, sizeof(prevName));

    strlcpy(config.data.wifiSSID, cfg["wifiSSID"] | "", sizeof(config.data.wifiSSID));
    keepOrSet(config.data.wifiPass, cfg["wifiPass"], sizeof(config.data.wifiPass));

    const char *nm = cfg["deviceName"] | "";
    char cleanName[sizeof(config.data.deviceName)];
    sanitizeDeviceName(nm, strlen(nm), cleanName, sizeof(cleanName));
    strlcpy(config.data.deviceName, cleanName, sizeof(config.data.deviceName));

    config.data.flags = cfg["flags"] | 0;
    config.data.manualBrightness = cfg["manualBrightness"] | 160;
    config.data.sleepTime = cfg["sleepTime"] | 2200;
    config.data.sleepLength = cfg["sleepLength"] | 90;
    config.data.signalBrightness = cfg["signalBrightness"] | 255;
    config.data.sunriseTime = cfg["sunriseTime"] | 700;
    config.data.sunriseLength = cfg["sunriseLength"] | 60;
    config.data.sunLength = cfg["sunLength"] | 30;
    config.data.sunriseBrightness = cfg["sunriseBrightness"] | 255;
    config.data.tzOffsetMinutes = cfg["tzOffsetMinutes"] | 180;

    config.data.soundStart = cfg["soundStart"] | 2300;
    config.data.soundEnd = cfg["soundEnd"] | 600;
    config.data.soundThreshold = cfg["soundThreshold"] | 60;
    config.data.soundBrightness = cfg["soundBrightness"] | 40;
    config.data.soundFadeInSec = cfg["soundFadeInSec"] | 3;
    config.data.soundHoldSec = cfg["soundHoldSec"] | 120;
    config.data.soundFadeOutSec = cfg["soundFadeOutSec"] | 20;

    config.data.mqttEnabled = cfg["mqttEnabled"] | false;
    strlcpy(config.data.mqttHost, cfg["mqttHost"] | "", sizeof(config.data.mqttHost));
    config.data.mqttPort = cfg["mqttPort"] | 1883;
    strlcpy(config.data.mqttUser, cfg["mqttUser"] | "", sizeof(config.data.mqttUser));
    keepOrSet(config.data.mqttPass, cfg["mqttPass"], sizeof(config.data.mqttPass));
    config.data.mqttIntervalSec = cfg["mqttIntervalSec"] | 0;

    config.save();
    configDirty = false;
    timeClient.setTimeOffset(config.data.tzOffsetMinutes * 60);
    // Смена брокера подхватывается на лету, перезагрузка не нужна
    mqtt.applyConfig();

    bool wifiChanged = strcmp(prevSSID, config.data.wifiSSID) != 0
                    || strcmp(prevPass, config.data.wifiPass) != 0
                    || strcmp(prevName, config.data.deviceName) != 0;

    JsonDocument resp;
    resp["type"] = "saveConfigResult";
    resp["success"] = true;
    resp["restarting"] = wifiChanged;
    resp["message"] = wifiChanged ? "Сохранено, перезагружаюсь..." : "Сохранено";
    wsSendJson(client, resp);

    // Не сразу: ответ уже поставлен в очередь отправки, и сброс
    // до её разбора оставил бы клиента без подтверждения.
    if (wifiChanged) needsRestart = true;
    else applyNightLight();
  }
  else if (strcmp(type, "restart") == 0) {
    needsRestart = true;
  }
  else if (strcmp(type, "confirmOta") == 0) {
    failsafe.confirm();
    JsonDocument resp;
    resp["type"] = "confirmOtaResult";
    resp["success"] = true;
    wsSendJson(client, resp);
  }
  else if (strcmp(type, "forgetWiFi") == 0) {
    // Аварийный выход, если сеть сменилась и портал недоступен
    // иначе как через точку доступа.
    config.data.wifiSSID[0] = '\0';
    config.data.wifiPass[0] = '\0';
    config.save();
    JsonDocument resp;
    resp["type"] = "saveConfigResult";
    resp["success"] = true;
    resp["restarting"] = true;
    resp["message"] = "Сеть забыта, поднимаю точку доступа...";
    wsSendJson(client, resp);
    needsRestart = true;
  }
}

// ==================== HTTP ====================
void setupHttpRoutes() {
  // SPA с LittleFS. Кэш отключён: после uploadfs страница должна
  // обновиться сразу, а LittleFS не хранит время записи файла,
  // поэтому опереться на ETag/Last-Modified нельзя.
  server.serveStatic("/", LittleFS, "/")
        .setDefaultFile("index.html")
        .setCacheControl("no-cache");

  // Снимок состояния обычным GET — удобно дёргать из Home Assistant
  // или curl, не поднимая WebSocket.
  server.on("/api.json", HTTP_GET, [](AsyncWebServerRequest *request) {
    JsonDocument doc;
    fillSystemState(doc);
    String json;
    serializeJson(doc, json);
    request->send(200, "application/json", json);
  });

  // Подтверждение прошивки обычным GET — на случай, если до кнопки в
  // интерфейсе не добраться
  server.on("/confirm", HTTP_GET, [](AsyncWebServerRequest *request) {
    failsafe.confirm();
    request->send(200, "text/plain", "Firmware confirmed");
  });

  server.on("/metrics", HTTP_GET, [](AsyncWebServerRequest *request) {
    String body;

    // Ради этой серии всё и затевалось: ночная активность в комнате.
    // level — мгновенный размах в момент опроса, сам по себе он почти
    // ничего не значит: Prometheus заходит раз в десятки секунд и
    // попадает в случайную миллисекунду. Реальную картину даёт peak —
    // максимум за последнюю минуту, поэтому график строить по нему.
    body += F("# HELP smartnightlight_sound_level Instant peak-to-peak from the microphone\n");
    body += F("# TYPE smartnightlight_sound_level gauge\n");
    body += F("smartnightlight_sound_level ");
    body += String(sound.level());
    body += F("\n");

    body += F("# HELP smartnightlight_sound_peak Loudest peak-to-peak over the last 60 s\n");
    body += F("# TYPE smartnightlight_sound_peak gauge\n");
    body += F("smartnightlight_sound_peak ");
    body += String(sound.peak());
    body += F("\n");

    // Срабатывания монотонно растут — counter, чтобы работали rate()
    // и increase(): «сколько раз ребёнок вставал за ночь»
    body += F("# HELP smartnightlight_sound_triggers_total Sound threshold crossings\n");
    body += F("# TYPE smartnightlight_sound_triggers_total counter\n");
    body += F("smartnightlight_sound_triggers_total ");
    body += String(sound.triggers());
    body += F("\n");

    // Микрофон, отдающий ровно ноль, не отличить от идеальной тишины
    // никак иначе. Без этой серии «шума не было» и «датчик не
    // подключён» на графике выглядят одинаково.
    body += F("# HELP smartnightlight_sound_sensor_present Microphone signal is above the noise floor\n");
    body += F("# TYPE smartnightlight_sound_sensor_present gauge\n");
    body += F("smartnightlight_sound_sensor_present ");
    body += String(sound.present() ? 1 : 0);
    body += F("\n");

    body += F("# HELP smartnightlight_brightness Current LED brightness 0..255\n");
    body += F("# TYPE smartnightlight_brightness gauge\n");
    body += F("smartnightlight_brightness ");
    body += String(currentBrightness);
    body += F("\n");

    // Режим — метка, а не число: значение всегда 1, а имя режима
    // лежит в лейбле. Так его можно фильтровать в запросах.
    body += F("# HELP smartnightlight_mode_info Active mode\n");
    body += F("# TYPE smartnightlight_mode_info gauge\n");
    body += F("smartnightlight_mode_info{mode=\"");
    body += nl::modeName(currentMode);
    body += F("\"} 1\n");

    body += F("# HELP smartnightlight_time_valid NTP time is available\n");
    body += F("# TYPE smartnightlight_time_valid gauge\n");
    body += F("smartnightlight_time_valid ");
    body += String(timeValid ? 1 : 0);
    body += F("\n");

    body += F("# HELP smartnightlight_wifi_rssi_dbm WiFi signal strength\n");
    body += F("# TYPE smartnightlight_wifi_rssi_dbm gauge\n");
    body += F("smartnightlight_wifi_rssi_dbm ");
    body += String(wifiConnected ? WiFi.RSSI() : 0);
    body += F("\n");

    body += F("# HELP smartnightlight_mqtt_connected MQTT broker connection\n");
    body += F("# TYPE smartnightlight_mqtt_connected gauge\n");
    body += F("smartnightlight_mqtt_connected ");
    body += String(mqtt.isConnected() ? 1 : 0);
    body += F("\n");

    body += F("# HELP smartnightlight_uptime_seconds System uptime\n");
    body += F("# TYPE smartnightlight_uptime_seconds gauge\n");
    body += F("smartnightlight_uptime_seconds ");
    body += String((millis() - bootMillis) / 1000);
    body += F("\n");

    body += F("# HELP smartnightlight_free_heap_bytes Free heap memory\n");
    body += F("# TYPE smartnightlight_free_heap_bytes gauge\n");
    body += F("smartnightlight_free_heap_bytes ");
    body += String(ESP.getFreeHeap());
    body += F("\n");

    // Фрагментация важнее свободного объёма: на ESP8266 падают не от
    // нехватки памяти, а от нехватки непрерывного куска. Без этих
    // двух серий такое не видно вовсе.
    body += F("# HELP smartnightlight_heap_fragmentation_percent Heap fragmentation\n");
    body += F("# TYPE smartnightlight_heap_fragmentation_percent gauge\n");
    body += F("smartnightlight_heap_fragmentation_percent ");
    body += String(ESP.getHeapFragmentation());
    body += F("\n");

    body += F("# HELP smartnightlight_heap_max_block_bytes Largest contiguous free block\n");
    body += F("# TYPE smartnightlight_heap_max_block_bytes gauge\n");
    body += F("smartnightlight_heap_max_block_bytes ");
    body += String(ESP.getMaxFreeBlockSize());
    body += F("\n");

    body += F("# HELP smartnightlight_ws_clients Connected WebSocket clients\n");
    body += F("# TYPE smartnightlight_ws_clients gauge\n");
    body += F("smartnightlight_ws_clients ");
    body += String(ws.count());
    body += F("\n");

    // Ноль — норма; рост означает, что команды с веб-интерфейса до
    // платы не доезжают
    body += F("# HELP smartnightlight_ws_rx_drops_total Dropped inbound WS chunks\n");
    body += F("# TYPE smartnightlight_ws_rx_drops_total counter\n");
    body += F("smartnightlight_ws_rx_drops_total ");
    body += String(wsRx.drops());
    body += F("\n");

    // Причина последнего сброса отличает штатную перезагрузку от
    // watchdog и исключения. Метка, а не число: значение всегда 1.
    body += F("# HELP smartnightlight_reset_info Reason of the last reset\n");
    body += F("# TYPE smartnightlight_reset_info gauge\n");
    body += F("smartnightlight_reset_info{reason=\"");
    body += ESP.getResetReason();
    body += F("\"} 1\n");

    request->send(200, "text/plain; version=0.0.4", body);
  });

  server.onNotFound([](AsyncWebServerRequest *request) {
    if (LittleFS.exists("/index.html")) {
      request->send(LittleFS, "/index.html", "text/html");
    } else {
      request->send(404, "text/plain",
                    "Frontend not uploaded: pio run -t uploadfs");
    }
  });
}
