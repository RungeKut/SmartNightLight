# Метрики для Prometheus

`GET /metrics` в формате Prometheus. Брокер не нужен, авторизации нет —
эндпоинт рассчитан на домашнюю сеть.

```yaml
# prometheus.yml
scrape_configs:
  - job_name: smartnightlight
    scrape_interval: 15s
    static_configs:
      - targets: ['192.168.88.20']
```

Лучше прописать IP, а не mDNS-имя: Prometheus резолвит цели системным
резолвером, а `.local` умеет не всякий.

## Ночная активность

Ради этого всё и затевалось.

| Метрика | Тип | Смысл |
|---------|-----|-------|
| `smartnightlight_sound_level` | gauge | мгновенный размах в момент опроса |
| `smartnightlight_sound_peak` | gauge | максимум за последние 60 секунд |
| `smartnightlight_sound_triggers_total{window="inside"}` | counter | события в ночном окне |
| `smartnightlight_sound_triggers_total{window="outside"}` | counter | события вне окна |
| `smartnightlight_sound_window_active` | gauge | 1 = ночное окно действует сейчас |
| `smartnightlight_sound_sensor_present` | gauge | 1 = микрофон отвечает |

**График стройте по `sound_peak`, а не по `sound_level`.** Prometheus
заходит раз в десятки секунд и попадает в случайную миллисекунду: в
мгновенном уровне ночной шорох виден только при феноменальном везении.
Пик за минуту собирается на устройстве из двенадцати пятисекундных
корзин, поэтому между опросами ничего не теряется.

Сколько раз ребёнок вставал за ночь:

```promql
increase(smartnightlight_sound_triggers_total{window="inside"}[8h])
```

Дневная активность в комнате — тот же запрос с `window="outside"`, а сумма
получается без указания метки вовсе:

```promql
sum(increase(smartnightlight_sound_triggers_total[24h]))
```

Метка, а не две отдельные метрики, именно ради этого: периоды и разделяются,
и складываются одним выражением.

`sound_window_active` полезно наложить на график фоном — видно, когда окно
вообще действовало, и не приходится держать расписание в голове.

Счётчик считает события, а не измерения: пока отклик идёт, повторный
шум его только продлевает. Подробности — в `docs/modules/sound.md`.

`sound_sensor_present` нужен, чтобы отличить тишину от оторванного
провода: без него «шума не было» и «датчик отвалился» выглядят на
графике одинаково.

## Что делает светильник

| Метрика | Тип | Смысл |
|---------|-----|-------|
| `smartnightlight_brightness` | gauge | фактическая яркость 0..255 |
| `smartnightlight_mode_info{mode="..."}` | gauge | активный режим, значение всегда 1 |
| `smartnightlight_time_valid` | gauge | пришло ли время с NTP |

Режим — метка, а не число: имя лежит в лейбле, значение всегда 1. Так
его можно фильтровать в запросах, и не нужно помнить, какой цифре
соответствует «рассвет».

## Здоровье платы

| Метрика | Тип |
|---------|-----|
| `smartnightlight_wifi_rssi_dbm` | gauge |
| `smartnightlight_mqtt_connected` | gauge |
| `smartnightlight_uptime_seconds` | gauge |
| `smartnightlight_free_heap_bytes` | gauge |
| `smartnightlight_heap_fragmentation_percent` | gauge |
| `smartnightlight_heap_max_block_bytes` | gauge |
| `smartnightlight_ws_clients` | gauge |
| `smartnightlight_ws_rx_drops_total` | counter |
| `smartnightlight_reset_info{reason="..."}` | gauge |

**Фрагментация важнее свободного объёма.** На ESP8266 падают не от
нехватки памяти, а от нехватки непрерывного куска: свободной кучи может
быть 15 КБ, а крупного блока не найтись. Без `heap_fragmentation_percent`
и `heap_max_block_bytes` это не видно вовсе.

`reset_info` отличает штатную перезагрузку от watchdog и исключения.
Значения `Exception` или `Hardware Watchdog` на графике означают, что
прошивка падает, — стоит смотреть `docs/pitfalls.md`.

`ws_rx_drops_total` в норме ноль. Рост означает, что команды с
веб-интерфейса до платы не доезжают.

## Аптайм как индикатор перезагрузок

Аптайм — gauge, он сбрасывается при перезагрузке. Незапланированные
рестарты видно так:

```promql
resets(smartnightlight_uptime_seconds[1h])
```

## Чего в метриках нет

Настроек. Расписание и пороги не меняются сами и в графиках не нужны —
они есть в `/api.json` и в MQTT. Метрики отдают только то, что меняется
во времени.
