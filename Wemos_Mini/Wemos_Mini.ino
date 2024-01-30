// получаем клики со страницы и значения с компонентов
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
GyverPortal ui;
byte switchState;
#define enableSleepTime 0x01
#define enableSignalSleepTime 0x02
#define enableAttenuationSleepTime 0x04
#define enableSunriseTime 0x08
#define FLAG5 0x10
#define FLAG6 0x20
#define FLAG7 0x40
#define FLAG8 0x80
bool eepromNeedChanged = false;
bool LEDNeedChanged = false;
time_t epochTime;
struct tm *ptm;
// Текущие дата и время
GPdate valDate;
GPtime valTime;
bool ledIsON = false;
byte brightnessLedIsON;
//Время отхода ко сну
uint16_t sleepTime;
uint32_t sleepTimespan;
byte lengthSleepTime;
//Сигнал "Пора спать"
byte brightnessSignalSleepTime;
//Затухание при засыпании
//Имитация рассвета
uint16_t sunriseTime;
byte lengthSunriseTime;
byte lengthSunTime;
byte maxBrightnessSunTime;

//Конструктор страницы
void build() {
  uint16_t tempTime;
  GP.BUILD_BEGIN(GP_DARK);
  GP.PAGE_TITLE("Умный Филин Ночник", "SmartNightLight");
  GP.TITLE("SmartNightLight или Умный Филин Ночник", "t1");
  GP.HR(); // Текущие дата и время
  GP.DATE("date", valDate); GP.BREAK();
  GP.TIME("time", valTime); GP.BREAK();
  GP.LABEL("Включить "); GP.SWITCH("ledIsON", ledIsON); GP.BREAK();
  GP.LABEL("&#128262;Яркость"); GP.BREAK();
  EEPROM.get(1, brightnessLedIsON);
  GP.SLIDER("brightnessLedIsON", brightnessLedIsON, 0, 255);
  GP.HR(); //Время отхода ко сну
  GP.LABEL("Время отхода ко сну"); GP.SWITCH("enableSleepTime", switchState & enableSleepTime); GP.BREAK();
  EEPROM.get(2, sleepTime);
  GP.TIME("sleepTime", GPtime(sleepTime / 100, sleepTime % 100, 0));
  GP.LABEL("Продолжительность отхода ко сну, мин.");
  EEPROM.get(4, lengthSleepTime);
  GP.SLIDER("lengthSleepTime", lengthSleepTime, 0, 255); GP.BREAK();
  sleepTimespan = sleepTime * 10000 + (lengthSleepTime / 60.0) * 100;
  GP.HR();//Сигнал "Пора спать"
  GP.LABEL("Сигнал \"Пора спать\""); GP.SWITCH("enableSignalSleepTime", switchState & enableSignalSleepTime); GP.BREAK();//Сигнал "Пора спать" за 5 минут до отхода ко сну
  GP.LABEL("&#128262;Яркость сигнала \"Пора спать\""); GP.BREAK();
  EEPROM.get(5, brightnessSignalSleepTime);
  GP.SLIDER("brightnessSignalSleepTime", brightnessSignalSleepTime, 0, 255); GP.BREAK();
  GP.HR(); //Затухание при засыпании линейно от начала до конца периода
  GP.LABEL("Затухание при засыпании"); GP.SWITCH("enableAttenuationSleepTime", switchState & enableAttenuationSleepTime); GP.BREAK();
  GP.HR(); //Имитация рассвета
  GP.LABEL("Имитация рассвета"); GP.SWITCH("enableSunriseTime", switchState & enableSunriseTime); GP.BREAK();
  EEPROM.get(6, sunriseTime);
  GP.TIME("sunriseTime", GPtime(sunriseTime / 100, sunriseTime % 100, 0));
  GP.LABEL("Продолжительность рассвета, мин."); //Нарастание яркости линейно от начала до конца периода до максимальной
  EEPROM.get(8, lengthSunriseTime);
  GP.SLIDER("lengthSunriseTime", lengthSunriseTime, 0, 255); GP.BREAK();
  GP.LABEL("Продолжительность после рассвета рассвета, мин."); //Сколько еще гореть на максимальной яркости
  EEPROM.get(9, lengthSunTime);
  GP.SLIDER("lengthSunTime", lengthSunTime, 0, 255); GP.BREAK();
  GP.LABEL("&#128262;Максимальная яркость рассвета"); GP.BREAK();
  EEPROM.get(10, maxBrightnessSunTime);
  GP.SLIDER("maxBrightnessSunTime", maxBrightnessSunTime, 0, 255); GP.BREAK();
  GP.HR();
  GP.BUILD_END();
}

// Получатель времени из интернета
#include <NTPClient.h>
//Так-же можно более детально настроить пул и задержку передачи.
#define NTP_OFFSET   60 * 60      // In seconds
#define NTP_INTERVAL 60 * 1000    // In miliseconds
#define NTP_ADDRESS  "europe.pool.ntp.org" //"ua.pool.ntp.org"  // change this to whatever pool is closest (see ntp.org)
// Set up the NTP UDP client
WiFiUDP ntpUDP; //Объект ntp
NTPClient timeClient(ntpUDP, NTP_ADDRESS, NTP_OFFSET, NTP_INTERVAL);

int ledPin = 2; //GPIO 0 (D3) 2-для отладки
void SetLedBrightness(byte brightness){
  analogWriteFreq(1000); //(100.. 40000 Гц)
  analogWriteResolution(8); //(4...16 бит)
  analogWrite(ledPin, brightness);
}

void WriteEeprom(){
  if (eepromNeedChanged){
    EEPROM.put(0, switchState); //Сохраняем состояние свичей
    EEPROM.commit();
    eepromNeedChanged = false;
  }
}

void setup() {
  analogWriteFreq(1000); //(100.. 40000 Гц)
  analogWriteResolution(8); //(4...16 бит)
  SetLedBrightness(0);
  timeClient.begin(); //Запускаем клиент времени
  timeClient.setTimeOffset(10800); //Указываем смещение по времени от Гринвича. Москва GMT+3 => 60*60*3 = 10800
  Serial.begin(115200);
  WiFi.mode(WIFI_STA);
  WiFi.hostname(DEV_NAME);
  WiFi.begin(AP_SSID, AP_PASS);
  while (WiFi.waitForConnectResult() != WL_CONNECTED) {
    Serial.println("Connection Failed! Rebooting...");
    delay(5000);
    ESP.restart();
  }

  // Port defaults to 8266
  ArduinoOTA.setPort(8266);

  // Hostname defaults to esp8266-[ChipID]
  ArduinoOTA.setHostname("SmartNightLight");

  // No authentication by default
  // ArduinoOTA.setPassword("admin");

  // Password can be set with it's md5 value as well
  // MD5(admin) = 21232f297a57a5a743894a0e4a801fc3
  // ArduinoOTA.setPasswordHash("21232f297a57a5a743894a0e4a801fc3");

  ArduinoOTA.onStart([]() {
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH) {
      type = "sketch";
    } else {  // U_FS
      type = "filesystem";
    }

    // NOTE: if updating FS this would be the place to unmount FS using FS.end()
    Serial.println("Start updating " + type);
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\nEnd");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) {
      Serial.println("Auth Failed");
    } else if (error == OTA_BEGIN_ERROR) {
      Serial.println("Begin Failed");
    } else if (error == OTA_CONNECT_ERROR) {
      Serial.println("Connect Failed");
    } else if (error == OTA_RECEIVE_ERROR) {
      Serial.println("Receive Failed");
    } else if (error == OTA_END_ERROR) {
      Serial.println("End Failed");
    }
  });
  ArduinoOTA.begin();
  Serial.println("Ready");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
  EEPROM.begin(4096); //Активируем EEPROM
  EEPROM.get(0, switchState); //Читаем состояния переключателей
  // подключаем конструктор и запускаем
  ui.attachBuild(build);
  ui.attach(action);
  ui.start();
}

void action() {
  // был клик по компоненту
  bool state;
  GPtime tempTime;
  if (ui.click()) {
    // Текущие дата и время
    if (ui.clickDate("date", valDate)) {
      Serial.print("Date: ");
      Serial.println(valDate.encode());
    }
    if (ui.clickTime("time", valTime)) {
      Serial.print("Time: ");
      Serial.println(valTime.encode());
    }
    if (ui.clickBool("ledIsON", ledIsON)) {
      Serial.print("Включен удаленно: ");
      Serial.println(ledIsON);
    }
    if (ui.clickInt("brightnessLedIsON", brightnessLedIsON)) {
      Serial.print("Яркость сигнала \"Пора спать\": ");
      EEPROM.put(1, brightnessLedIsON);
      Serial.println(brightnessLedIsON);
    }
    //Время отхода ко сну
    if (ui.clickBool("enableSleepTime", state)) {
      if (state) switchState |= enableSleepTime; else switchState &= ~enableSleepTime;
      Serial.print("Отход ко сну: ");
      Serial.println(switchState & enableSleepTime);
    }
    if (ui.clickTime("sleepTime", tempTime)) {
      sleepTime = tempTime.hour * 100 + tempTime.minute;
      EEPROM.put(2, sleepTime);
      Serial.print("Время отхода ко сну: ");
      Serial.println(tempTime.encode());
    }
    if (ui.clickInt("lengthSleepTime", lengthSleepTime)) {
      EEPROM.put(4, lengthSleepTime);
      Serial.print("Продолжительность отхода ко сну, мин.: ");
      Serial.println(lengthSleepTime);
    }
    sleepTimespan = sleepTime * 10000 + (lengthSleepTime / 60.0) * 100;
    //Сигнал "Пора спать"
    if (ui.clickBool("enableSignalSleepTime", state)) {
      if (state) switchState |= enableSignalSleepTime; else switchState &= ~enableSignalSleepTime;
      Serial.print("Сигнал \"Пора спать\": ");
      Serial.println(switchState & enableSignalSleepTime);
    }
    if (ui.clickInt("brightnessSignalSleepTime", brightnessSignalSleepTime)) {
      EEPROM.put(5, brightnessSignalSleepTime);
      Serial.print("Яркость сигнала \"Пора спать\": ");
      Serial.println(brightnessSignalSleepTime);
    }
    //Затухание при засыпании
    if (ui.clickBool("enableAttenuationSleepTime", state)) {
      if (state) switchState |= enableAttenuationSleepTime; else switchState &= ~enableAttenuationSleepTime;
      Serial.print("Затухание при засыпании: ");
      Serial.println(switchState & enableAttenuationSleepTime);
    }
    //Имитация рассвета
    if (ui.clickBool("enableSunriseTime", state)) {
      if (state) switchState |= enableSunriseTime; else switchState &= ~enableSunriseTime;
      Serial.print("Имитация рассвета: ");
      Serial.println(switchState & enableSunriseTime);
    }
    if (ui.clickTime("sunriseTime", tempTime)) {
      sunriseTime = tempTime.hour * 100 + tempTime.minute;
      EEPROM.put(6, sunriseTime);
      Serial.print("Время Имитация рассвета: ");
      Serial.println(tempTime.encode());
    }
    if (ui.clickInt("lengthSunriseTime", lengthSunriseTime)) {
      EEPROM.put(8, lengthSunriseTime);
      Serial.print("Продолжительность рассвета, мин.: ");
      Serial.println(lengthSunriseTime);
    }
    if (ui.clickInt("lengthSunTime", lengthSunTime)) {
      EEPROM.put(9, lengthSunTime);
      Serial.print("Продолжительность после рассвета рассвета, мин.: ");
      Serial.println(lengthSunTime);
    }
    if (ui.clickInt("maxBrightnessSunTime", maxBrightnessSunTime)) {
      EEPROM.put(10, maxBrightnessSunTime);
      Serial.print("Максимальная яркость рассвета: ");
      Serial.println(maxBrightnessSunTime);
    }
    Serial.print("Состояние переключателей: ");
    Serial.println(switchState);
    eepromNeedChanged = true;
    LEDNeedChanged = true;
  }
}

void WiFiupd(){
    if (WiFi.status() == WL_CONNECTED) //Проверка подключения к Wifi
    {
      // update the NTP client and get the UNIX UTC timestamp 
      timeClient.update();
      valTime = GPtime(timeClient.getHours(), timeClient.getMinutes(), timeClient.getSeconds());
      epochTime = timeClient.getEpochTime();
      ptm = gmtime((time_t *)&epochTime);
      valDate = GPdate(ptm -> tm_year + 1900, ptm -> tm_mon + 1, ptm -> tm_mday);
    }
    else //Пробуем подключиться заново
    {
      delay(5000);
      Serial.println("Переподключаемся к Wifi");
      WiFi.begin(AP_SSID, AP_PASS);
      while (WiFi.waitForConnectResult() != WL_CONNECTED) {
        Serial.println("Connection Failed! Rebooting...");
        delay(5000);
        ESP.restart();
      }
    }
}

bool checkTimespan(uint32_t timespan){
  if (timespan > 0 | timespan < 23602360){
    uint16_t t0 = ptm->tm_hour * 100 + ptm->tm_min;
    uint16_t t1 = timespan / 10000;
    uint16_t t2 = timespan % 10000;
    return (t1 < t2) ? ((t0 >= t1) && (t0 < t2)) : !((t0 >= t2) && (t1 > t0));
  }
  return false;
}

void loop() {
  ArduinoOTA.handle();
  ui.tick();
  WiFiupd();
  //delay(1000);
  if (LEDNeedChanged){
    if (ledIsON) SetLedBrightness(brightnessLedIsON);
    else if (switchState & enableSleepTime){
      if (checkTimespan(sleepTimespan)){
        
      }
    }
    else SetLedBrightness(0);
    LEDNeedChanged = false;
  }
}
