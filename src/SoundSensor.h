/******************************************************************
 * SoundSensor.h — микрофон GY-MAX4466 на A0
 *
 * Модуль не записывает звук и не различает его источник. Он меряет
 * размах сигнала (peak-to-peak) за короткое окно — этого достаточно,
 * чтобы отличить тишину спящей комнаты от того, что ребёнок встал.
 *
 * ПОЧЕМУ РАЗМАХ, А НЕ УРОВЕНЬ
 *
 * MAX4466 отдаёт переменный сигнал вокруг половины питания (около
 * 512 в отсчётах АЦП). Мгновенное значение analogRead() само по себе
 * не говорит ничего: в тишине оно тоже гуляет вокруг середины.
 * Разница между максимумом и минимумом за окно — и есть громкость.
 *
 * ОДИН АЦП НА ВСЮ ПЛАТУ
 *
 * У ESP8266 единственный аналоговый вход. На Wemos D1 mini он заведён
 * через делитель, поэтому модуль питается от 3.3 В и его выход идёт
 * на A0 напрямую. Других потребителей АЦП в проекте нет.
 *
 * ЧУВСТВИТЕЛЬНОСТЬ
 *
 * У GY-MAX4466 есть подстроечный резистор усиления. Порог в настройках
 * и этот резистор настраиваются вместе: сначала крутим резистор так,
 * чтобы в тишине уровень был у нуля, а хлопок давал заметный размах,
 * потом ставим порог между ними. Текущий уровень и пик видны в
 * веб-интерфейсе — подбирать удобнее по ним, чем вслепую.
 ******************************************************************/

#ifndef SoundSensor_h
#define SoundSensor_h

#include <Arduino.h>

// Окно измерения. 20 мс — это примерно один период самого низкого
// звука, который нас интересует, и при этом loop() не встаёт
// настолько, чтобы это было заметно.
#define SOUND_WINDOW_MS 20
// Между измерениями: чаще незачем, а АЦП при работающем WiFi лучше
// не дёргать без нужды.
#define SOUND_PERIOD_MS 100

// Кольцо пиков для скользящего максимума. Prometheus опрашивает раз в
// десятки секунд, и мгновенный уровень в момент опроса почти ничего
// не значит — в график должен попадать пик за интервал.
#define SOUND_PEAK_BUCKETS 12
#define SOUND_BUCKET_MS 5000       // 12 * 5 с = окно 60 с

// Ниже этого пика за минуту считаем, что микрофона на входе нет.
//
// Свободный вход A0 не даёт чистого нуля: на плате он ловит наводки, и
// размах всё равно гуляет на единицу-другую. Проверено на этой плате
// без микрофона — пик держался на 2. Живой MAX4466 даже в тишине
// шумит собственным усилителем заметно выше, поэтому порог различает
// «тихо в комнате» и «вход болтается».
#define SOUND_PRESENCE_MIN 4

class SoundSensor {
private:
  uint8_t  _pin;
  uint16_t _level;        // размах в последнем окне
  uint16_t _peakWindow;   // максимум за последние 60 секунд
  uint16_t _buckets[SOUND_PEAK_BUCKETS];
  uint8_t  _bucket;
  uint32_t _lastBucketMs;
  uint32_t _lastSampleMs;
  uint32_t _triggers;     // событий внутри ночного окна
  uint32_t _triggersOut;  // событий за его пределами

public:
  SoundSensor()
    : _pin(A0), _level(0), _peakWindow(0), _bucket(0),
      _lastBucketMs(0), _lastSampleMs(0), _triggers(0), _triggersOut(0) {
    for (uint8_t i = 0; i < SOUND_PEAK_BUCKETS; i++) _buckets[i] = 0;
  }

  void begin() {
    pinMode(_pin, INPUT);
    _lastBucketMs = millis();
    _lastSampleMs = millis();
  }

  // Измеряет размах за окно. Возвращает true, если измерение было
  // выполнено в этот раз.
  bool tick(uint32_t now) {
    if (now - _lastSampleMs < SOUND_PERIOD_MS) return false;
    _lastSampleMs = now;

    uint16_t lo = 1023, hi = 0;
    uint32_t until = millis() + SOUND_WINDOW_MS;
    uint16_t samples = 0;
    while ((int32_t)(millis() - until) < 0) {
      uint16_t v = analogRead(_pin);
      if (v < lo) lo = v;
      if (v > hi) hi = v;
      samples++;
      // analogRead на ESP8266 сам по себе не кормит watchdog, а окно
      // целиком проводим в этом цикле
      if ((samples & 0x3F) == 0) yield();
    }

    _level = (hi >= lo) ? (hi - lo) : 0;

    rotateBuckets(now);
    if (_level > _buckets[_bucket]) _buckets[_bucket] = _level;
    recomputePeak();
    return true;
  }

  // Порог превышен в последнем измерении
  bool exceeded(uint16_t threshold) const {
    return threshold > 0 && _level >= threshold;
  }

  // Два счётчика, а не один: ночная активность и дневная — разные
  // величины. Смешивать их означало бы потерять и ту и другую, потому
  // что днём шум в детской стоит почти постоянно.
  void countTrigger(bool insideWindow) {
    if (insideWindow) _triggers++;
    else _triggersOut++;
  }

  uint16_t level() const { return _level; }
  uint16_t peak() const { return _peakWindow; }
  uint32_t triggers() const { return _triggers; }
  uint32_t triggersOutside() const { return _triggersOut; }

  // Оценка по текущему пику, а не по факту "когда-либо был сигнал":
  // отвалившийся провод так виден сразу, а не до перезагрузки.
  bool present() const { return _peakWindow >= SOUND_PRESENCE_MIN; }

private:
  void rotateBuckets(uint32_t now) {
    while (now - _lastBucketMs >= SOUND_BUCKET_MS) {
      _lastBucketMs += SOUND_BUCKET_MS;
      _bucket = (_bucket + 1) % SOUND_PEAK_BUCKETS;
      _buckets[_bucket] = 0;
    }
  }

  void recomputePeak() {
    uint16_t m = 0;
    for (uint8_t i = 0; i < SOUND_PEAK_BUCKETS; i++) {
      if (_buckets[i] > m) m = _buckets[i];
    }
    _peakWindow = m;
  }
};

#endif
