/*
This sketch demonstrates recording of standard WAV files that can be played on any device that supports WAVs. The recording
uses a single ended input from any of the analog input pins. Uses AVCC (5V) reference currently.

Requirements:
Class 4 or 6 SD Card
Audio Input Device (Microphone, etc)
Arduino Uno,Nano, Mega, etc.

Steps:
1. Edit pcmConfig.h
    a: On Uno or non-mega boards, #define buffSize 128. May need to increase.
    b: Uncomment #define ENABLE_RECORDING and #define BLOCK_COUNT 10000UL

2. Usage is as below. See <a href="https://github.com/TMRh20/TMRpcm/wiki/Advanced-Features#wiki-recording-audio" title="https://github.com/TMRh20/TMRpcm/wiki/Advanced-Features#wiki-recording-audio" rel="nofollow">https://github.com/TMRh20/TMRpcm/wiki/Advanced-Features#wiki-recording-a...</a> for
   additional informaiton.

Notes: Recording will not work in Multi Mode.
Performance is very dependant on SD write speed, and memory used.
Better performance may be seen using the SdFat library. See included example for usage.
Running the Arduino from a battery or filtered power supply will reduce noise.
*/

#include <SD.h>
#include <SPI.h>
#include <TMRpcm.h>
#include <EEPROM.h>

#define ENABLE_DEBUG        //Включение вывода отладочной информации через Serial. Только для отладки! Закоментировать эту строку перед заливкой в готовое устройство!

#define SD_ChipSelectPin 10  //

#define ledStart 2         /* PIN LED Start */
#define ledStop 3          /* PIN LED Stop */
#define MicPin A1          // Аналоговый пин, к которму подключен микрофон

char NameRecord[10];        // Имя нового - записываемого файла на SD-карту
int RecordNumber;           // Номер записи - храним в EEPROM. в диапазоне от 0 до 32767
byte Recording = 0;         // Переменная определяет идет сейчас запись или нет
                            //-----------------------------------------------
int RecInterval = 5;        //! Минимальная продолжительность записи в секундах !
                            //-----------------------------------------------
unsigned long TimeInterval = 0; // Переменная для вычисления времени
int MaxAnalogPinValue = 1000;   // Уровень сигнала на аналоговом входе при котором произойдет старт записи
                                // Значение подбирается индивидуально! (100 - 1023)
int SaveADC1 = 0;
int SaveADC2 = 0;

unsigned int sample;
unsigned int signalMax = 0;
unsigned int signalMin = 256;
unsigned int peakToPeak = 0;
#define peakToPeakMinLevel 200   // Уровень срабатывания на звук. Значение подбирается индивидуально!
                                // Максимальное значение - 255

TMRpcm audio;   // create an object for use in this sketch 

void setup() {
    #if defined (ENABLE_DEBUG)
        Serial.begin(9600);
    #endif
    
    pinMode(ledStart, OUTPUT);
    pinMode(ledStop, OUTPUT);
    //audio.speakerPin = 11; //5,6,11 or 46 on Mega, 9 on Uno, Nano, etc
    pinMode(10,OUTPUT);  //Pin pairs: 9,10 Mega: 5-2,6-7,11-12,46-45
    
    if (!SD.begin(SD_ChipSelectPin)) {  
        digitalWrite(ledStop, LOW);
        digitalWrite(ledStart, LOW);
        return;
    }else{
        digitalWrite(ledStop, LOW);
        digitalWrite(ledStart, HIGH);
    }
    analogReference(EXTERNAL);
    // The audio library needs to know which CS pin to use for recording
    audio.CSPin = SD_ChipSelectPin;
    
    RecordNumber = EEPROM.read(0);
    RecInterval = RecInterval * 1000;
}


void loop() {
    if (Recording == 0){
        int AnR = analogRead(MicPin);
        #if defined (ENABLE_DEBUG)
            Serial.print("Analog level: ");
            Serial.println(AnR);
        #endif
        if (AnR >= MaxAnalogPinValue) {
            #if defined (ENABLE_DEBUG)
                Serial.print("Start recording! File: ");
                Serial.print(RecordNumber+1);
                Serial.println(".wav");
            #endif
            StartRec();
        }
    }
    
    if (Recording == 1){
        sample = ADCH;
        if (sample < 256)
        {
            if (sample > signalMax)
            {
                signalMax = sample;  // save just the max levels
            }
            else if (sample < signalMin)
            {
                signalMin = sample;  // save just the min levels
            }
        }
        #if defined (ENABLE_DEBUG)
            Serial.print("Max MIC signal: ");
            Serial.println(signalMax);
        #endif
        if (millis() - TimeInterval >= RecInterval) {
            peakToPeak = signalMax - signalMin;
            signalMax = 0;
            signalMin = 256;
            #if defined (ENABLE_DEBUG)
                Serial.print("peakToPeak = ");
                Serial.println(peakToPeak);
            #endif
            if (peakToPeak < peakToPeakMinLevel) {
                analogRead(MicPin);
                StopRec();
                #if defined (ENABLE_DEBUG)
                    Serial.println("Recording stopped");
                #endif
            }else{
                TimeInterval = millis()-1;
                #if defined (ENABLE_DEBUG)
                    Serial.println("Continue recording...");
                #endif
            }
        }
    }
}

void StartRec() {                   // begin recording process
    SaveADC1 = ADCSRA;
    SaveADC2 = ADCSRB;
    Recording = 1;
    digitalWrite(ledStop, HIGH);
    digitalWrite(ledStart, LOW);
    RecordNumber++;
    if (RecordNumber > 32766)RecordNumber = 0;      // небольшое огриничение в номерации файлов
    EEPROM.write(0, RecordNumber);                  // Сохранение в EEPROM номера последнего аудиофайла
    TimeInterval = millis();                        // Запоминаем  millis для отсчета времени записи
    sprintf(NameRecord,"%d.wav", RecordNumber);     // создаем название файла из номера и расширения ".wav"
    audio.startRecording(NameRecord, 16000, MicPin, 0);// Старт записи
}

void StopRec() {                    // stop recording process, close file
    audio.stopRecording(NameRecord);// Стоп записи
    digitalWrite(ledStop, LOW);
    digitalWrite(ledStart, HIGH);
    Recording = 0;
    ADCSRA = SaveADC1;
    ADCSRB = SaveADC2;
}
