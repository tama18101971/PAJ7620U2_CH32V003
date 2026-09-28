# INTEGRATION — перенос с APDS-9960 на PAJ7620U2

Документ описывает замену драйвера `APDS-9960_CH32V003` на `PAJ7620U2_CH32V003`
в существующем проекте (в частности — в `E:\GitProjects\DriverMD6`).
API новой библиотеки повторяет API старой, поэтому перенос сводится к
переименованию вызовов и **одному обязательному изменению логики** в пути
пробуждения из standby (раздел 4).

## 1. Соответствие API

| APDS-9960 (`apds9960.h`) | PAJ7620U2 (`paj7620.h`) | Комментарий |
|---|---|---|
| `gesture_t` | `gesture_t` | Первые 5 значений (`NONE`/`LEFT`/`RIGHT`/`UP`/`DOWN` = 0..4) совпадают. Добавлены `FORWARD`, `BACKWARD`, `CW`, `CCW`, `WAVE` |
| `apds_init()` | `paj_init()` | + I2C wake-up, проверка готовности, калибровка порогов PS |
| `apds_sleep()` | `paj_sleep()` | Снимается бит Enable, конфигурация сохраняется |
| `apds_wakeup()` | `paj_wakeup()` | Выполняет I2C wake-up — **обязателен после `paj_shutdown()`** |
| `apds_shutdown()` | `paj_shutdown()` | Enable = 0 + команда suspend (IDD_SUS ~15 мкА) |
| `apds_available()` | `paj_available()` | Читает флаги жеста; чтение сбрасывает их в датчике, результат кэшируется |
| `apds_readGesture()` | `paj_readGesture()` | Блокирующая: кэш → ожидание до `PAJ_GESTURE_TIMEOUT_MS` |
| `apds_readProximity(uint8_t*)` | `paj_readProximity(uint8_t*)` | Требует режима proximity — см. раздел 5 |
| `apds_readStatus(uint8_t*)` | `paj_readStatus(uint8_t*)` | Читает регистр состояния автомата датчика (0..2) |
| `apds_getLastError()` | `paj_getLastError()` | Коды 0/1/3/4/5 совпадают, 2 (`FIFO_OVERFLOW`) не используется |
| `apds_getLastI2CStatus()` | `paj_getLastI2CStatus()` | Коды i2c.h те же |
| `apds_getReinitCount()` | `paj_getReinitCount()` | Счётчик переинициализаций после сбоев I2C |
| `apds_recalibrate()` | `paj_recalibrate()` | Калибрует пороги proximity |
| `apds_enableInterrupt()` | `paj_enableInterrupt()` | Пишет маски `0x41`/`0x42` |
| `apds_disableInterrupt()` | `paj_disableInterrupt()` | Обнуляет маски |
| `apds_clearInterrupt()` | `paj_clearInterrupt()` | Снимает вывод INT (чтение флагов) |
| `int_config.h` → `apds_exti_init()` … | `paj_int_config.h` → `paj_exti_init()` … | Тот же вектор `EXTI7_0_IRQHandler`, тот же вывод PC3 |
| `g_apds_int_flag` | `g_paj_int_flag` | Флаг ISR |

| Макрос конфигурации | APDS | PAJ |
|---|---|---|
| Режим прерываний | `APDS_INT_MODE` | `PAJ_INT_MODE` |
| Владелец EXTI-вектора | `APDS_PROVIDE_EXTI_ISR` | `PAJ_PROVIDE_EXTI_ISR` |
| Свой SysTick | `APDS_OWN_SYSTICK` | `PAJ_OWN_SYSTICK` |
| Калибровка | `APDS_ENABLE_CALIBRATION`, `APDS_CAL_*` | `PAJ_ENABLE_CALIBRATION`, `PAJ_CAL_*` |
| Таймаут жеста | `APDS_GESTURE_TIMEOUT_MS` | `PAJ_GESTURE_TIMEOUT_MS` |
| Повторы | `APDS_RETRY_LIMIT` | `PAJ_RETRY_LIMIT` |
| Отладка | `APDS9960_DEBUG` | `PAJ7620U2_DEBUG` |
| Пороги proximity | `APDS_PROX_THRESHOLD`, `APDS_GESTURE_EXIT_TH` | `PAJ_PROX_THRESHOLD`, `PAJ_PROX_EXIT_TH` |
| — | — | `PAJ_GESTURE_MASK`, `PAJ_WAVE_MASK`, `PAJ_PS_MASK` (новые) |
| — | — | `PAJ_GESTURE_ROTATE` 0..3 (новое: повёрнутый модуль) |
| — | — | `PAJ_ENABLE_PS_MODE` (новое: режим proximity) |

Параметры APDS, не имеющие смысла для PAJ7620U2 (усиление/ток LED, пауза
gesture engine, пороги FIFO, фильтрация пакетов, чувствительность жеста),
отсутствуют: gesture engine и AE работают внутри датчика.

## 2. Зависимость и адрес датчика

* Библиотека `I2C-CH32V003` — та же, что и у APDS-драйвера (пин `#v7.1.0`).
* Адрес на шине: `0x39` (APDS9960) → **`0x73`** (PAJ7620U2).
* Распиновка сохраняется: `PC1 = SDA`, `PC2 = SCL`, `PC3 = INT` (open-drain,
  подтяжка вверх), UART TX — `PD5`.

## 3. Правки в проекте (на примере DriverMD6)

### 3.1 `platformio.ini`

```ini
lib_deps =
    https://github.com/tama18101971/PAJ7620U2_CH32V003.git#v1.0.0   ; было APDS-9960_CH32V003#v1.3.0
    ...

build_flags =
    ...
    -DPAJ_INT_MODE=1              ; было -DAPDS_INT_MODE=1
    -DPAJ_PROVIDE_EXTI_ISR=0      ; было -DAPDS_PROVIDE_EXTI_ISR=0
```

### 3.2 Замена имён (механическая правка по всему проекту)

| Было | Стало | Файлы DriverMD6 |
|---|---|---|
| `#include "apds9960.h"` | `#include "paj7620.h"` | `src/main.c`, `src/standby.c`, `src/ui.c` |
| `#include "int_config.h"` | `#include "paj_int_config.h"` | `src/main.c`, `src/ui.c` |
| `apds_init/apds_available/apds_readGesture/apds_clearInterrupt` | `paj_init/paj_available/paj_readGesture/paj_clearInterrupt` | `src/main.c` |
| `apds_enableInterrupt/apds_disableInterrupt/apds_shutdown/apds_wakeup` | `paj_enableInterrupt/paj_disableInterrupt/paj_shutdown/paj_wakeup` | `src/main.c`, `src/standby.c`, `src/ui.c` |
| `apds_handle_exti()` | `paj_handle_exti()` | `src/standby.c` (ISR `EXTI7_0_IRQHandler`) |
| `apds_ok` | `paj_ok` | `src/main.c`, `src/standby.c`, `src/ui.c` (своё имя проекта) |
| `APDS_PORT` / `APDS_PIN` | `PAJ_INT_PORT` / `PAJ_INT_PIN` или оставить `GPIOC` / `GPIO_Pin_3` | `include/standby.h` |
| `standby_set_apds_exti()` | имя можно оставить прежним (внутреннее API проекта) | `src/standby.c` |

`apds_int_pending` — флаг самого проекта, а не драйвера: имя менять не нужно.

## 4. Обязательное изменение логики: выход из suspend

`paj_shutdown()` переводит датчик в **suspend** (datasheet, «vi. Enter Suspend
mode»): `Enable = 0` + запись `0x01` в регистр `0x03`. В suspend датчик не
отвечает на I2C, и для продолжения работы требуется I2C wake-up-команда
(«vii. Resume Gesture»). У APDS-9960 `apds_shutdown()` только снимал бит PON
при сохранённой конфигурации, поэтому повторный `apds_wakeup()` в цикле
выключения питания не требовался.

**DriverMD6, `src/standby.c`, путь пробуждения из standby** (около строки 275):

```c
if (i2c_init(400000) == I2C_OK && CFG->on_gst) {
    paj_wakeup();                    /* выход из suspend: I2C wake-up + Enable */
    standby_set_apds_exti(1U);
    paj_enableInterrupt();
    ...
}
```

Без этого вызова датчик останется в suspend: `paj_enableInterrupt()` вернёт
`PAJ_ERR_I2C`, а `paj_available()` — `false`. Диагностика: `paj_getLastError()`
и `paj_getReinitCount()`.

Так же стоит проверить `enter_standby_sequence()` (`src/standby.c`, ≈ строка 336):
перед уходом в standby вызывается `paj_shutdown()` — порядок «shutdown →
(standby) → wakeup → enable» сохраняется.

## 5. Прочие отличия, о которых нужно знать

1. **Чтение флагов съедает жест.** Регистры `0x43`/`0x44` сбрасываются при
   чтении, поэтому `paj_available()` декодирует жест и кэширует его —
   последующий `paj_readGesture()` вернёт именно этот жест. Цикл
   `while (paj_available()) paj_readGesture();` остаётся корректным.
   `paj_clearInterrupt()` также сохраняет найденный жест, поэтому
   «сбросить прерывание, потом проверить» безопасно (в отличие от APDS, где
   `apds_clearInterrupt()` был практически no-op).
2. **Датчик хранит только последний жест.** Если два жеста успеют произойти
   между опросами, один будет потерян. Для потока «INT → чтение» потери нет.
3. **Новые значения `gesture_t`.** `switch` без `default` перестанет ловить
   `FORWARD`/`BACKWARD`/`CW`/`CCW`/`WAVE` — добавьте явные ветки или `default`.
4. **`PAJ_ERR_FIFO_OVERFLOW` (2) не возвращается** — у PAJ7620U2 нет
   host-FIFO. Макрос оставлен с прежним значением для совместимости кода.
5. **Проверка идентификации другая.** У PAJ7620U2 нет регистра PART_ID:
   драйвер проверяет регистр состояния `0x00 == 0x20` после I2C wake-up и
   подтверждает включение чтением `bank1 0x72`. Ответ `0xFF` даёт
   `PAJ_ERR_INVALID_ID`.
6. **Proximity требует переключения режима.** В режиме жестов датчик PS-данные
   не обновляет (`paj_readProximity()` вернёт 0). Для работы с proximity:
   `paj_setProximityMode()` → `paj_recalibrate()` → … → `paj_setGestureMode()`.
   В DriverMD6 proximity не используется — вызывать ничего не нужно.
7. **Повёрнутый модуль.** Если датчик установлен с наклоном 90/180/270°,
   задайте `PAJ_GESTURE_ROTATE=1..3`: драйвер переназначит направления
   программно, приложение продолжит получать физические направления.

## 6. Проверка после переноса

1. `pio run` — сборка должна пройти без предупреждений.
2. `pio run -t upload`, `pio device monitor` (115200): в примере печатается
   `PAJ7620U2 ready.` и названия жестов.
3. Проверить `paj_getLastError()`/`paj_getLastI2CStatus()` при ошибке: для
   `PAJ_ERR_SENSOR_HANG` — ненулевой `paj_getReinitCount()`.
4. Проверить цикл standby → пробуждение: после выхода из standby датчик снова
   распознаёт жесты (раздел 4).
5. Сопоставить размер прошивки: при `-DPAJ_ENABLE_CALIBRATION=0` и без
   `printf` драйвер занимает ~1.5 КБ флэша и 15 байт RAM
   (эталонная сборка `examples/basic` с LTO: RAM 508 Б, Flash 10792 Б).
