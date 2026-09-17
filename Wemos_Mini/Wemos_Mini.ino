// SmartNightLight на Wemos D1 Mini
#define DEV_NAME "SmartNightLight"
#define AP_SSID "75kV-2G"
#define AP_PASS "qwert35967200LOX"

#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <time.h>
#include <EEPROM.h>
#include <GyverPortal.h>
#include <NTPClient.h>

GyverPortal ui;

// Флаги в switchState
#define enableSleepTime             0x01
#define enableSignalSleepTime       0x02
#define enableAttenuationSleepTime  0x04
#define enableSunriseTime           0x08
// остальные флаги не используются, но оставлены для совместимости
#define FLAG5 0x10
#define FLAG6 0x20
#define FLAG7 0x40
#define FLAG8 0x80

byte switchState = 0;          // битовая маска включённых режимов
bool eepromNeedChanged = false;
time_t epochTime;
struct tm *ptm;

// Текущие дата и время
GPdate valDate;
GPtime valTime;

uint32_t eepromTimespan;
bool ledIsON = false;
byte brightnessLedIsON = 160;

// Время отхода ко сну
uint16_t sleepTime = 1000;   // 10:00
uint32_t sleepTimespan;      // интервал: start * 10000 + end
byte lengthSleepTime = 90;   // 90 минут

// Сигнал "Пора спать"
byte brightnessSignalSleepTime = 255;
uint32_t signalSleepTimespan;

// Затухание при засыпании
class Parabola {
public:
  float a, b, c;
  const float minBrightness = 3.0;

  void initDimSleep(float length, float brightness) {
    if (brightness < minBrightness) brightness = minBrightness;
    a = (brightness - minBrightness) / (length * length);
    b = -2.0f * a * length;
    c = brightness;
  }

  void initDimSunrise(float length, float brightness) {
    if (brightness < minBrightness) brightness = minBrightness;
    b = (brightness - minBrightness) / (length - (length * length / 2.0f));
    a = -b / 2.0f;
    c = minBrightness;
  }

  float y(float x) {
    return a * x * x + b * x + c;
  }
};

Parabola dimSleep;
Parabola dimSunrise;

// Имитация рассвета
uint16_t sunriseTime = 700;           // 07:00
uint32_t sunriseTimespan;
byte lengthSunriseTime = 60;          // 60 минут на рассвет
byte lengthSunTime = 30;              // ещё 30 минут на полной яркости
byte maxBrightnessSunTime = 255;

// Конструктор страницы
void build() {
  GP.BUILD_BEGIN(GP_DARK);
  GP.PAGE_TITLE("Умный Филин Ночник", "SmartNightLight");
  GP.TITLE("SmartNightLight или Умный Филин Ночник", "t1");
  GP.HR();

  GP.DATE("date", valDate); GP.BREAK();
  GP.TIME("time", valTime); GP.BREAK();

  GP.LABEL("Включить "); GP.SWITCH("ledIsON", ledIsON); GP.BREAK();
  GP.LABEL("&#128262;Яркость"); GP.BREAK();
  GP.SLIDER("brightnessLedIsON", brightnessLedIsON, 0, 255);

  GP.HR(); // Время отхода ко сну
  GP.LABEL("Время отхода ко сну");
  GP.SWITCH("enableSleepTime", switchState & enableSleepTime); GP.BREAK();
  GP.TIME("sleepTime", GPtime(sleepTime / 100, sleepTime % 100, 0));
  GP.LABEL("Продолжительность отхода ко сну, мин.");
  GP.SLIDER("lengthSleepTime", lengthSleepTime, 0, 255); GP.BREAK();

  GP.HR(); // Сигнал "Пора спать"
  GP.LABEL("Сигнал \"Пора спать\"");
  GP.SWITCH("enableSignalSleepTime", switchState & enableSignalSleepTime); GP.BREAK();
  GP.LABEL("&#128262;Яркость сигнала \"Пора спать\""); GP.BREAK();
  GP.SLIDER("brightnessSignalSleepTime", brightnessSignalSleepTime, 0, 255); GP.BREAK();

  GP.HR(); // Затухание при засыпании
  GP.LABEL("Затухание при засыпании");
  GP.SWITCH("enableAttenuationSleepTime", switchState & enableAttenuationSleepTime); GP.BREAK();

  GP.HR(); // Имитация рассвета
  GP.LABEL("Имитация рассвета");
  GP.SWITCH("enableSunriseTime", switchState & enableSunriseTime); GP.BREAK();
  GP.TIME("sunriseTime", GPtime(sunriseTime / 100, sunriseTime % 100, 0));
  GP.LABEL("Продолжительность рассвета, мин.");
  GP.SLIDER("lengthSunriseTime", lengthSunriseTime, 0, 255); GP.BREAK();
  GP.LABEL("Продолжительность после рассвета, мин.");
  GP.SLIDER("lengthSunTime", lengthSunTime, 0, 255); GP.BREAK();
  GP.LABEL("&#128262;Максимальная яркость рассвета"); GP.BREAK();
  GP.SLIDER("maxBrightnessSunTime", maxBrightnessSunTime, 0, 255); GP.BREAK();

  GP.HR();
  GP.BUILD_END();
}

// NTP клиент
#define NTP_OFFSET   (60 * 60 * 3)     // GMT+3 (Москва)
#define NTP_INTERVAL (60 * 1000)       // 1 минута
#define NTP_ADDRESS  "europe.pool.ntp.org"

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, NTP_ADDRESS, NTP_OFFSET, NTP_INTERVAL);

int ledPin = 0; // GPIO0 (D3)
byte LEDNeedChanged = 0;

void SetLedBrightness(byte brightness) {
  if (LEDNeedChanged != brightness) {
    analogWrite(ledPin, brightness);
    LEDNeedChanged = brightness;
  }
}

// Вспомогательные функции времени

// Возвращает количество минут от startTime до currentTime (с учётом 00:00)
uint32_t minutesSince(uint16_t startTime, uint16_t currentTime) {
  uint32_t startMin = (startTime / 100) * 60 + (startTime % 100);
  uint32_t currMin  = (currentTime / 100) * 60 + (currentTime % 100);
  if (currMin < startMin) currMin += 24 * 60; // перешли через полночь
  return currMin - startMin;
}

// Создаёт timespan как start * 10000 + end
uint32_t createTimespan(uint16_t startTime, uint16_t durationMin) {
  uint32_t totalStartMin = (startTime / 100) * 60 + (startTime % 100);
  uint32_t totalEndMin = totalStartMin + durationMin;
  if (totalEndMin >= 24 * 60) totalEndMin -= 24 * 60;

  uint16_t endHour = (totalEndMin / 60) % 24;
  uint16_t endMin  = totalEndMin % 60;
  uint16_t endTime = endHour * 100 + endMin;

  return (uint32_t)startTime * 10000 + endTime;
}

// Проверяет, попадает ли текущее время в интервал [start, end]
bool checkTimespan(uint32_t timespan) {
  uint16_t t0 = ptm->tm_hour * 100 + ptm->tm_min;
  uint16_t t1 = timespan / 10000;      // начало
  uint16_t t2 = timespan % 10000;      // конец

  if (t1 <= t2) {
    return (t0 >= t1 && t0 <= t2);
  } else {
    return (t0 >= t1 || t0 <= t2);
  }
}

// Используется для задержки записи в EEPROM (не влияет на логику времени)
uint32_t createDelayTimespan(uint16_t startTime, uint32_t delayTime) {
  uint32_t totalStartMin = (startTime / 100) * 60 + (startTime % 100);
  uint32_t totalEndMin = totalStartMin + delayTime;
  if (totalEndMin >= 24 * 60) totalEndMin -= 24 * 60;

  uint16_t endHour = (totalEndMin / 60) % 24;
  uint16_t endMin  = totalEndMin % 60;
  uint16_t endTime = endHour * 100 + endMin;

  return (uint32_t)endTime * 10000 + startTime;
}

void WriteEeprom() {
  if (eepromNeedChanged) {
    if (checkTimespan(eepromTimespan)) {
      EEPROM.put(0, switchState);
      EEPROM.commit();
      eepromNeedChanged = false;
    }
  }
}

void ReadEeprom() {
  EEPROM.get(0, switchState);
  EEPROM.get(1, brightnessLedIsON);
  EEPROM.get(2, sleepTime);
  EEPROM.get(4, lengthSleepTime);
  sleepTimespan = createTimespan(sleepTime, lengthSleepTime);
  signalSleepTimespan = createTimespan(sleepTime, 5); // вспышки — первые 5 минут интервала
  EEPROM.get(5, brightnessSignalSleepTime);
  dimSleep.initDimSleep(lengthSleepTime, brightnessSignalSleepTime);
  EEPROM.get(6, sunriseTime);
  EEPROM.get(8, lengthSunriseTime);
  EEPROM.get(9, lengthSunTime);
  sunriseTimespan = createTimespan(sunriseTime, lengthSunriseTime + lengthSunTime);
  EEPROM.get(10, maxBrightnessSunTime);
  dimSunrise.initDimSunrise(lengthSunriseTime, maxBrightnessSunTime);
}

void setup() {
  pinMode(ledPin, OUTPUT);
  analogWriteFreq(1000);      // 100..40000 Гц
  analogWriteResolution(8);   // иначе диапазон PWM 0..1023, и 255 даст четверть яркости
  SetLedBrightness(255);
  delay(100);
  SetLedBrightness(0);

  timeClient.begin();
  timeClient.setTimeOffset(10800); // +3 часа

  Serial.begin(115200);
  WiFi.mode(WIFI_STA);
  WiFi.hostname(DEV_NAME);
  WiFi.begin(AP_SSID, AP_PASS);
  while (WiFi.waitForConnectResult() != WL_CONNECTED) {
    Serial.println("Connection Failed! Rebooting...");
    delay(5000);
    ESP.restart();
  }

  ArduinoOTA.setHostname("SmartNightLight");
  ArduinoOTA.begin();

  EEPROM.begin(512); // 512 байт достаточно
  ReadEeprom();

  ui.attachBuild(build);
  ui.attach(action);
  ui.start();

  Serial.println("Ready");
  Serial.print("IP: "); Serial.println(WiFi.localIP());
}

void action() {
  bool state;
  GPtime tempTime;
  if (!ui.click()) return;

  if (ui.clickBool("ledIsON", ledIsON)) {
    // ничего не сохраняем — это volatile состояние
  }

  if (ui.clickInt("brightnessLedIsON", brightnessLedIsON)) {
    EEPROM.put(1, brightnessLedIsON);
    eepromNeedChanged = true;
    eepromTimespan = createDelayTimespan(valTime.hour * 100 + valTime.minute, 360);
  }

  if (ui.clickBool("enableSleepTime", state)) {
    if (state) switchState |= enableSleepTime; else switchState &= ~enableSleepTime;
    eepromNeedChanged = true;
    eepromTimespan = createDelayTimespan(valTime.hour * 100 + valTime.minute, 360);
  }

  if (ui.clickTime("sleepTime", tempTime)) {
    sleepTime = tempTime.hour * 100 + tempTime.minute;
    sleepTimespan = createTimespan(sleepTime, lengthSleepTime);
    signalSleepTimespan = createTimespan(sleepTime, 5);
    EEPROM.put(2, sleepTime);
    eepromNeedChanged = true;
    eepromTimespan = createDelayTimespan(valTime.hour * 100 + valTime.minute, 360);
  }

  if (ui.clickInt("lengthSleepTime", lengthSleepTime)) {
    EEPROM.put(4, lengthSleepTime);
    sleepTimespan = createTimespan(sleepTime, lengthSleepTime);
    dimSleep.initDimSleep(lengthSleepTime, brightnessSignalSleepTime);
    eepromNeedChanged = true;
    eepromTimespan = createDelayTimespan(valTime.hour * 100 + valTime.minute, 360);
  }

  if (ui.clickBool("enableSignalSleepTime", state)) {
    if (state) switchState |= enableSignalSleepTime; else switchState &= ~enableSignalSleepTime;
    eepromNeedChanged = true;
    eepromTimespan = createDelayTimespan(valTime.hour * 100 + valTime.minute, 360);
  }

  if (ui.clickInt("brightnessSignalSleepTime", brightnessSignalSleepTime)) {
    EEPROM.put(5, brightnessSignalSleepTime);
    dimSleep.initDimSleep(lengthSleepTime, brightnessSignalSleepTime);
    eepromNeedChanged = true;
    eepromTimespan = createDelayTimespan(valTime.hour * 100 + valTime.minute, 360);
  }

  if (ui.clickBool("enableAttenuationSleepTime", state)) {
    if (state) switchState |= enableAttenuationSleepTime; else switchState &= ~enableAttenuationSleepTime;
    eepromNeedChanged = true;
    eepromTimespan = createDelayTimespan(valTime.hour * 100 + valTime.minute, 360);
  }

  if (ui.clickBool("enableSunriseTime", state)) {
    if (state) switchState |= enableSunriseTime; else switchState &= ~enableSunriseTime;
    eepromNeedChanged = true;
    eepromTimespan = createDelayTimespan(valTime.hour * 100 + valTime.minute, 360);
  }

  if (ui.clickTime("sunriseTime", tempTime)) {
    sunriseTime = tempTime.hour * 100 + tempTime.minute;
    sunriseTimespan = createTimespan(sunriseTime, lengthSunriseTime + lengthSunTime);
    EEPROM.put(6, sunriseTime);
    eepromNeedChanged = true;
    eepromTimespan = createDelayTimespan(valTime.hour * 100 + valTime.minute, 360);
  }

  if (ui.clickInt("lengthSunriseTime", lengthSunriseTime)) {
    EEPROM.put(8, lengthSunriseTime);
    sunriseTimespan = createTimespan(sunriseTime, lengthSunriseTime + lengthSunTime);
    dimSunrise.initDimSunrise(lengthSunriseTime, maxBrightnessSunTime);
    eepromNeedChanged = true;
    eepromTimespan = createDelayTimespan(valTime.hour * 100 + valTime.minute, 360);
  }

  if (ui.clickInt("lengthSunTime", lengthSunTime)) {
    EEPROM.put(9, lengthSunTime);
    sunriseTimespan = createTimespan(sunriseTime, lengthSunriseTime + lengthSunTime);
    eepromNeedChanged = true;
    eepromTimespan = createDelayTimespan(valTime.hour * 100 + valTime.minute, 360);
  }

  if (ui.clickInt("maxBrightnessSunTime", maxBrightnessSunTime)) {
    EEPROM.put(10, maxBrightnessSunTime);
    dimSunrise.initDimSunrise(lengthSunriseTime, maxBrightnessSunTime);
    eepromNeedChanged = true;
    eepromTimespan = createDelayTimespan(valTime.hour * 100 + valTime.minute, 360);
  }
}

bool stepSignalSleepTime = false;

uint32_t lastWiFiRetry = 0;

void WiFiupd() {
  if (WiFi.status() != WL_CONNECTED) {
    // Не блокируем loop(): OTA, портал и сам светильник должны работать,
    // пока роутер недоступен. Повторяем попытку раз в 30 секунд.
    uint32_t now = millis();
    if (now - lastWiFiRetry > 30000) {
      lastWiFiRetry = now;
      Serial.println("WiFi lost! Reconnecting...");
      WiFi.begin(AP_SSID, AP_PASS);
    }
  }
  else {
    timeClient.update();
  }

  // getEpochTime() отсчитывает время по millis() от последней синхронизации,
  // поэтому часы идут и без сети, а ptm никогда не остаётся нулевым.
  epochTime = timeClient.getEpochTime();
  ptm = gmtime(&epochTime);

  valTime = GPtime(timeClient.getHours(), timeClient.getMinutes(), timeClient.getSeconds());
  valDate = GPdate(ptm->tm_year + 1900, ptm->tm_mon + 1, ptm->tm_mday);
}

void loop() {
  ArduinoOTA.handle();
  ui.tick();
  WiFiupd();
  WriteEeprom();

  uint16_t currentTime = valTime.hour * 100 + valTime.minute;

  if (ledIsON) {
    SetLedBrightness(brightnessLedIsON);
  }
  else if ((switchState & enableSleepTime) && checkTimespan(sleepTimespan)) {
    if ((switchState & enableSignalSleepTime) && checkTimespan(signalSleepTimespan)) {
      // Мигаем "Пора спать"
      static unsigned long lastToggle = 0;
      if (millis() - lastToggle > 200) {
        stepSignalSleepTime = !stepSignalSleepTime;
        SetLedBrightness(stepSignalSleepTime ? brightnessSignalSleepTime : 0);
        lastToggle = millis();
      }
    }
    else {
      if (switchState & enableAttenuationSleepTime) {
        uint32_t elapsed = minutesSince(sleepTime, currentTime);
        if (elapsed > lengthSleepTime) elapsed = lengthSleepTime;
        int brightness = (int)dimSleep.y(elapsed);
        if (brightness < 0) brightness = 0;
        if (brightness > 255) brightness = 255;
        SetLedBrightness(brightness);
      }
      else {
        SetLedBrightness(brightnessLedIsON);
      }
    }
  }
  else if ((switchState & enableSunriseTime) && checkTimespan(sunriseTimespan)) {
    uint32_t elapsed = minutesSince(sunriseTime, currentTime);
    uint16_t totalSunDuration = lengthSunriseTime + lengthSunTime;
    if (elapsed > totalSunDuration) elapsed = totalSunDuration;
    int brightness = (int)dimSunrise.y(elapsed);
    if (brightness < 0) brightness = 0;
    if (brightness > 255) brightness = 255;
    SetLedBrightness(brightness);
  }
  else {
    SetLedBrightness(0);
  }

  delay(50);
}
