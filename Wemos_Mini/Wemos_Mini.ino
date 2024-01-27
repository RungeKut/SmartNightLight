// получаем клики со страницы и значения с компонентов
#define DEV_NAME "SmartNightLight"
#define AP_SSID "75kV-2G"
#define AP_PASS "qwert35967200LOX"

#include <GyverPortal.h>
GyverPortal ui;

// Текущие дата и время
GPdate valDate;
GPtime valTime;
bool ledIsON;
int brightnessLedIsON;
//Сигнал "Пора спать"
bool enableSignalSleepTime;
int brightnessSignalSleepTime;
GPtime timeSignalSleepTime;

//Конструктор страницы
void build() {
  GP.BUILD_BEGIN(GP_DARK);
  GP.PAGE_TITLE("Умный Ночник", "SmartNightLight");
  GP.TITLE("SmartNightLight или Умный Ночник", "t1");
  GP.HR(); // Текущие дата и время
  GP.DATE("date", valDate); GP.BREAK();
  GP.TIME("time", valTime); GP.BREAK();
  GP.LABEL("Включить ");
  GP.SWITCH("ledIsON", ledIsON); GP.BREAK();
  GP.SLIDER("brightnessLedIsON", brightnessLedIsON, 0, 1023);
  GP.HR(); //Сигнал "Пора спать"
  GP.LABEL("Сигнал \"Пора спать\" ");
  GP.SWITCH("enableSignalSleepTime", enableSignalSleepTime); GP.BREAK();
  GP.SLIDER("brightnessSignalSleepTime", brightnessSignalSleepTime, 0, 1023); GP.BREAK();
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

int ledPin = 0; //GPIO 2 (D3)
void SetLedBrightness(int brightness){
  analogWriteFreq(1000); //(100.. 40000 Гц)
  analogWriteResolution(10); //(4...16 бит)
  analogWrite(ledPin, brightness);
}

void setup() {
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

  // подключаем конструктор и запускаем
  ui.attachBuild(build);
  ui.attach(action);
  ui.start();
}

void action() {
  // был клик по компоненту
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
    //Сигнал "Пора спать"
    if (ui.clickBool("enableSignalSleepTime", enableSignalSleepTime)) {
      Serial.print("Сигнал \"Пора спать\": ");
      Serial.println(enableSignalSleepTime);
    }
    if (ui.clickInt("brightnessSignalSleepTime", brightnessSignalSleepTime)) {
      Serial.print("Яркость сигнала \"Пора спать\": ");
      Serial.println(brightnessSignalSleepTime);
      SetLedBrightness(brightnessSignalSleepTime);
    }
    if (ui.clickTime("timeSignalSleepTime", timeSignalSleepTime)) {
      Serial.print("Время срабатывания сигнала \"Пора спать\": ");
      Serial.println(timeSignalSleepTime.encode());
    }
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
        WiFi.begin(AP_SSID, AP_PASS);
    }
}

void loop() {
  ui.tick();
  WiFiupd();
  delay(1000);
}
