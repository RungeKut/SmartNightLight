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

// ==================== Логика светильника ====================
void applyNightLight() {
  nl::Settings s = config.toSettings();
  bool blinkOn = ((millis() / BLINK_PERIOD_MS) % 2) == 0;

  nl::Result r = nl::compute(s, nowHhmm, manualOn, blinkOn);

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

  timeClient.begin();
  timeClient.setTimeOffset(config.data.tzOffsetMinutes * 60);

  if (!connectToWiFi()) startAPMode();

  if (!LittleFS.begin()) {
    Log.println(F("[FS] LittleFS не смонтирована — залейте фронтенд: pio run -t uploadfs"));
  }

  ArduinoOTA.setHostname(deviceName);
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
  applyNightLight();
  flushConfig();
  handleSnapshots(now);

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

  doc["uptime"] = (millis() - bootMillis) / 1000;
  doc["heap"] = ESP.getFreeHeap();
  doc["ws_clients"] = ws.count();
  doc["ws_drops"] = wsRx.drops();
  doc["config_pending"] = configDirty;
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
  // Пароль от WiFi клиенту намеренно не отдаём
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

    config.save();
    configDirty = false;
    timeClient.setTimeOffset(config.data.tzOffsetMinutes * 60);

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

  server.onNotFound([](AsyncWebServerRequest *request) {
    if (LittleFS.exists("/index.html")) {
      request->send(LittleFS, "/index.html", "text/html");
    } else {
      request->send(404, "text/plain",
                    "Frontend not uploaded: pio run -t uploadfs");
    }
  });
}
