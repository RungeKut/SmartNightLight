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
time_t epochTime;
struct tm *ptm;
// Текущие дата и время
GPdate valDate;
GPtime valTime;
uint32_t eepromTimespan;
bool ledIsON = false;
byte brightnessLedIsON = 160;
//Время отхода ко сну
uint16_t sleepTime = 1000;
uint32_t sleepTimespan;
byte lengthSleepTime = 90;
//Сигнал "Пора спать"
byte brightnessSignalSleepTime = 255;
uint32_t signalSleepTimespan;
//Затухание при засыпании
class Parabola{
  public:
    float x1, y1, x2, y2, x3, y3, a, b, c;
    void init(byte length, byte brightness){
      x1 = 5;
      y1 = brightness;
      x2 = length / 2;
      y2 = brightness / 4;
      x3 = length;
      y3 = 4;
      calc();
    }
    void calc(){
      a=(y3-(x3*(y2-y1)+x2*y1-x1*y2)/(x2-x1))/(x3*(x3-x1-x2)+x1*x2);
      b=(y2-y1)/(x2-x1)-a*(x1+x2);
      c=(x2*y1-x1*y2)/(x2-x1)+a*x1*x2;
    }
    float y(float x){
      return a*x*x+b*x+c;
    }
};
Parabola dimSleep; 
//Имитация рассвета
uint16_t sunriseTime = 700;
uint32_t sunriseTimespan;
byte lengthSunriseTime = 60;
byte lengthSunTime = 30;
byte maxBrightnessSunTime = 255;

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
  sleepTimespan = createTimespan(sleepTime, lengthSleepTime);
  signalSleepTimespan = createTimespan(sleepTime, 5);
  GP.HR();//Сигнал "Пора спать"
  GP.LABEL("Сигнал \"Пора спать\""); GP.SWITCH("enableSignalSleepTime", switchState & enableSignalSleepTime); GP.BREAK();//Сигнал "Пора спать" за 5 минут до отхода ко сну
  GP.LABEL("&#128262;Яркость сигнала \"Пора спать\""); GP.BREAK();
  EEPROM.get(5, brightnessSignalSleepTime);
  GP.SLIDER("brightnessSignalSleepTime", brightnessSignalSleepTime, 0, 255); GP.BREAK();
  GP.HR(); //Затухание при засыпании линейно от начала до конца периода
  GP.LABEL("Затухание при засыпании"); GP.SWITCH("enableAttenuationSleepTime", switchState & enableAttenuationSleepTime); GP.BREAK();
  dimSleep.init(lengthSleepTime, brightnessSignalSleepTime);
  GP.HR(); //Имитация рассвета
  GP.LABEL("Имитация рассвета"); GP.SWITCH("enableSunriseTime", switchState & enableSunriseTime); GP.BREAK();
  EEPROM.get(6, sunriseTime);
  GP.TIME("sunriseTime", GPtime(sunriseTime / 100, sunriseTime % 100, 0));
  GP.LABEL("Продолжительность рассвета, мин."); //Нарастание яркости линейно от начала до конца периода до максимальной
  EEPROM.get(8, lengthSunriseTime);
  GP.SLIDER("lengthSunriseTime", lengthSunriseTime, 0, 255); GP.BREAK();
  GP.LABEL("Продолжительность после рассвета, мин."); //Сколько еще гореть на максимальной яркости
  EEPROM.get(9, lengthSunTime);
  GP.SLIDER("lengthSunTime", lengthSunTime, 0, 255); GP.BREAK();
  sunriseTimespan = createTimespan(sunriseTime, lengthSunriseTime + lengthSunTime);
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

int ledPin = 0; //GPIO 0 (D3) 2-для отладки
byte LEDNeedChanged;
void SetLedBrightness(byte brightness){
  if (LEDNeedChanged != brightness){
    analogWriteFreq(1000); //(100.. 40000 Гц)
    analogWriteResolution(8); //(4...16 бит)
    analogWrite(ledPin, brightness);
    LEDNeedChanged = brightness;
  }
}

void WriteEeprom(){
  if (eepromNeedChanged){
    if (checkTimespan(eepromTimespan)) //360мин - записывать через 6 часов после последнего изменения
    {
      EEPROM.put(0, switchState); //Сохраняем состояние свичей
      EEPROM.commit();
      eepromNeedChanged = false;
    }
  }
}

void setup() {
  analogWriteFreq(1000); //(100.. 40000 Гц)
  analogWriteResolution(8); //(4...16 бит)
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
  SetLedBrightness(0);
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
    }
    if (ui.clickTime("time", valTime)) {
    }
    if (ui.clickBool("ledIsON", ledIsON)) {
    }
    if (ui.clickInt("brightnessLedIsON", brightnessLedIsON)) {
      EEPROM.put(1, brightnessLedIsON);
      eepromNeedChanged = true;
      eepromTimespan = createDelayTimespan(valTime.hour * 100 + valTime.minute, 360);
    }
    //Время отхода ко сну
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
      dimSleep.init(lengthSleepTime, brightnessSignalSleepTime);
      eepromNeedChanged = true;
      eepromTimespan = createDelayTimespan(valTime.hour * 100 + valTime.minute, 360);
    }
    //Сигнал "Пора спать"
    if (ui.clickBool("enableSignalSleepTime", state)) {
      if (state) switchState |= enableSignalSleepTime; else switchState &= ~enableSignalSleepTime;
      eepromNeedChanged = true;
      eepromTimespan = createDelayTimespan(valTime.hour * 100 + valTime.minute, 360);
    }
    if (ui.clickInt("brightnessSignalSleepTime", brightnessSignalSleepTime)) {
      EEPROM.put(5, brightnessSignalSleepTime);
      dimSleep.init(lengthSleepTime, brightnessSignalSleepTime);
      eepromNeedChanged = true;
      eepromTimespan = createDelayTimespan(valTime.hour * 100 + valTime.minute, 360);
    }
    //Затухание при засыпании
    if (ui.clickBool("enableAttenuationSleepTime", state)) {
      if (state) switchState |= enableAttenuationSleepTime; else switchState &= ~enableAttenuationSleepTime;
      eepromNeedChanged = true;
      eepromTimespan = createDelayTimespan(valTime.hour * 100 + valTime.minute, 360);
    }
    //Имитация рассвета
    if (ui.clickBool("enableSunriseTime", state)) {
      if (state) switchState |= enableSunriseTime; else switchState &= ~enableSunriseTime;
      eepromNeedChanged = true;
      eepromTimespan = createDelayTimespan(valTime.hour * 100 + valTime.minute, 360);
    }
    if (ui.clickTime("sunriseTime", tempTime)) {
      sunriseTime = tempTime.hour * 100 + tempTime.minute;
      EEPROM.put(6, sunriseTime);
      eepromNeedChanged = true;
      eepromTimespan = createDelayTimespan(valTime.hour * 100 + valTime.minute, 360);
    }
    if (ui.clickInt("lengthSunriseTime", lengthSunriseTime)) {
      EEPROM.put(8, lengthSunriseTime);
      eepromNeedChanged = true;
      eepromTimespan = createDelayTimespan(valTime.hour * 100 + valTime.minute, 360);
    }
    if (ui.clickInt("lengthSunTime", lengthSunTime)) {
      EEPROM.put(9, lengthSunTime);
      eepromNeedChanged = true;
      eepromTimespan = createDelayTimespan(valTime.hour * 100 + valTime.minute, 360);
    }
    sunriseTimespan = createTimespan(sunriseTime, lengthSunriseTime + lengthSunTime);
    if (ui.clickInt("maxBrightnessSunTime", maxBrightnessSunTime)) {
      EEPROM.put(10, maxBrightnessSunTime);
      eepromNeedChanged = true;
      eepromTimespan = createDelayTimespan(valTime.hour * 100 + valTime.minute, 360);
    }
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

uint32_t createTimespan(uint16_t startTime, uint16_t lengthTime){
  uint32_t result = (startTime / 100) * 100;
  uint16_t temp = lengthTime + startTime % 100;
  while (temp >= 60){
    if (result / 100 >= 23){
      result = result % 100; //Если будет полночь то оставляем только минуты
    }
    else{
      result += 100; //Прибавляем час
    }
    temp -= 60; //Отнимаем 60 минут
  }
  return startTime * 10000 + result + temp; //Собираем все в кучу
}

uint32_t createDelayTimespan(uint16_t startTime, uint32_t delayTime){
  uint32_t result = (startTime / 100) * 100;
  uint32_t temp = delayTime + startTime % 100;
  while (temp >= 60){
    if (result / 100 >= 23){
      result = result % 100; //Если будет полночь то оставляем только минуты
    }
    else{
      result += 100; //Прибавляем час
    }
    temp -= 60; //Отнимаем 60 минут
  }
  return (result + temp) * 10000 + startTime; //Собираем все в кучу
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

uint32_t totalMinutes(uint32_t timespan){
  if (timespan > 0 | timespan < 23602360){
    uint16_t t1 = timespan / 10000;
    uint8_t t1h = t1 / 100;
    uint8_t t1m = t1 % 100;
    uint16_t t2 = timespan % 10000;
    uint8_t t2h = t2 / 100;
    uint8_t t2m = t2 % 100;
    return (t1 < t2) ? ((t2h * 60 + t2m) - (t1h * 60 + t1m)) : (24 * 60 - (t1h * 60 + t1m) + (t2h * 60 + t2m));
  }
  return 0;
}

bool stepSignalSleepTime = false;
void loop() {
  ArduinoOTA.handle();
  ui.tick();
  WiFiupd();
  WriteEeprom();
  delay(50);
  
  if (ledIsON){ //Если включен принудительно
    SetLedBrightness(brightnessLedIsON);
    return;
  }
  if ((switchState & enableSleepTime) && (checkTimespan(sleepTimespan))){ //Если включен режим отход ко сну и внутри интервала отхода ко сну
    if ((switchState & enableSignalSleepTime) && (checkTimespan(signalSleepTimespan))){ //Если активен сигнал "Пора спать" и внутри интервала "Пора спать" (5мин)
      delay(200); //Мигаем
      if (stepSignalSleepTime){
        SetLedBrightness(brightnessSignalSleepTime);
        stepSignalSleepTime = false;
      }
      else{
        SetLedBrightness(0);
        stepSignalSleepTime = true;
      }
    }
    else{
      if (switchState & enableAttenuationSleepTime){ //Затухание
        uint32_t countMinutes = totalMinutes((valTime.hour * 100 + valTime.minute) * 10000 + sleepTimespan % 10000);
        SetLedBrightness((int)dimSleep.y(countMinutes));
      }
      else
        SetLedBrightness(brightnessLedIsON);
    }
    return;
  }
  if ((switchState & enableSunriseTime) && (checkTimespan(sunriseTimespan))){ //Если включен рассвет и мы внутри его интервала

  }
  SetLedBrightness(0);
}
