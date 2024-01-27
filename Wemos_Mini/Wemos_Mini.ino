// получаем клики со страницы и значения с компонентов
#define DEV_NAME "SmartNightLight"
#define AP_SSID "75kV-2G"
#define AP_PASS "qwert35967200LOX"

#include <EEPROM.h>
#include <GyverPortal.h>
GyverPortal ui;
byte switchState;
#define enableSignalSleepTime 0x01
#define BUT_DOWN_FLAG              0x02
#define BUT_FORWARD_FLAG           0x04
#define BUT_BACKWARD_FLAG          0x08
#define BUT_STRONG_FLAG            0x10
#define BUT_WEAK_FLAG              0x20
#define INITIALIZATION_FLAG        0x40
#define STOP_FLAG                  0x80
bool eepromNeedChanged = false;
bool LEDNeedChanged = false;
// Текущие дата и время
GPdate valDate;
GPtime valTime;
bool ledIsON = false;
byte brightnessLedIsON;
//Сигнал "Пора спать"
byte brightnessSignalSleepTime;
GPtime timeSignalSleepTime;

//Конструктор страницы
void build() {
  GP.BUILD_BEGIN(GP_DARK);
  GP.PAGE_TITLE("Умный Филин Ночник", "SmartNightLight");
  GP.TITLE("SmartNightLight или Умный Филин Ночник", "t1");
  GP.HR(); // Текущие дата и время
  GP.DATE("date", valDate); GP.BREAK();
  GP.TIME("time", valTime); GP.BREAK();
  GP.LABEL("Включить ");
  GP.SWITCH("ledIsON", ledIsON); GP.BREAK();
  EEPROM.get(1, brightnessLedIsON);
  GP.SLIDER("brightnessLedIsON", brightnessLedIsON, 0, 255);
  GP.HR(); //Сигнал "Пора спать"
  GP.LABEL("Сигнал \"Пора спать\" ");
  GP.SWITCH("enableSignalSleepTime", switchState & enableSignalSleepTime); GP.BREAK();
  GP.SLIDER("brightnessSignalSleepTime", brightnessSignalSleepTime, 0, 255); GP.BREAK();
  GP.TIME("timeSignalSleepTime", timeSignalSleepTime);
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
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
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
    //Сигнал "Пора спать"
    if (ui.clickBool("enableSignalSleepTime", state)) {
      if (state) switchState |= enableSignalSleepTime; else switchState &= ~enableSignalSleepTime;
      Serial.print("Сигнал \"Пора спать\": ");
      Serial.println(switchState & enableSignalSleepTime);
    }
    if (ui.clickInt("brightnessSignalSleepTime", brightnessSignalSleepTime)) {
      Serial.print("Яркость сигнала \"Пора спать\": ");
      Serial.println(brightnessSignalSleepTime);
    }
    if (ui.clickTime("timeSignalSleepTime", timeSignalSleepTime)) {
      Serial.print("Время срабатывания сигнала \"Пора спать\": ");
      Serial.println(timeSignalSleepTime.encode());
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
      time_t epochTime = timeClient.getEpochTime();
      struct tm *ptm = localtime((time_t *)&epochTime);
      valDate = GPdate(ptm -> tm_year + 1900, ptm -> tm_mon + 1, ptm -> tm_mday);
    }
    else //Пробуем подключиться заново
    {
      Serial.println("Переподключаемся к Wifi");
      WiFi.begin(AP_SSID, AP_PASS);
    }
}

void loop() {
  ui.tick();
  WiFiupd();
  delay(1000);
  if (LEDNeedChanged){
    if (ledIsON) SetLedBrightness(brightnessLedIsON); else SetLedBrightness(0);
    LEDNeedChanged = false;
  }
}
