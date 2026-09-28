/*
 * paj7620_config.h — единая конфигурация драйвера PAJ7620U2.
 *
 * Этот файл включается и приложением, и paj7620.c. Переопределяйте значения
 * через build_flags PlatformIO, например:
 *   build_flags = -Isrc -DPAJ_GESTURE_MASK=0x0F -DPAJ_GESTURE_ROTATE=2
 *
 * #define в main.c перед #include "paj7620.h" не меняет уже отдельно
 * скомпилированный paj7620.c и потому не является поддерживаемым способом
 * настройки драйвера.
 */

#ifndef PAJ7620_CONFIG_H
#define PAJ7620_CONFIG_H

/* ---------------------------------------------------------------------------
 * Отладочный вывод
 *
 * Определите PAJ7620U2_DEBUG при сборке (-DPAJ7620U2_DEBUG), чтобы включить
 * printf из драйвера. Стоит ~2400 байт флэша — в промышленной прошивке
 * оставляйте выключенным.
 * ------------------------------------------------------------------------- */

/* ---------------------------------------------------------------------------
 * Маски прерываний (REG_INT1_EN 0x41 / REG_INT2_EN 0x42, банк 0)
 *
 * Бит = 1: соответствующий жест вызывает прерывание на выводе INT.
 * ------------------------------------------------------------------------- */

/* Маска жестов: биты [7:0] = CCW, CW, backward, forward, right, left, down, up
 * (см. GES_* в paj7620_regs.h). 0xFF — реагировать на все 8 жестов. */
#ifndef PAJ_GESTURE_MASK
#define PAJ_GESTURE_MASK                0xFF
#endif

/* Прерывание по жесту "взмах рукой" (REG_INT2_EN бит 0): 0/1. */
#ifndef PAJ_WAVE_MASK
#define PAJ_WAVE_MASK                   1
#endif

/* Прерывание по proximity (REG_INT2_EN бит 1): 0/1.
 * По умолчанию выключено: датчик не переводится в режим proximity, и PS-данные
 * в режиме жестов не обновляются (datasheet: "Only functional at proximity
 * detection mode"). Включайте только вместе с paj_setProximityMode(). */
#ifndef PAJ_PS_MASK
#define PAJ_PS_MASK                     0
#endif

/* Поворот модуля на 0/90/180/270 градусов по часовой стрелке (0..3).
 * Datasheet: "If rotate the gesture module, the direction of gesture
 * detection interrupt flag (Bank0 0x43) needs to be re-mapped". Драйвер
 * выполняет переназначение программно, чтобы приложение всегда получало
 * физическое направление жеста. Влияет также на переназначение CW/CCW:
 * при повороте на 90/270 градусов они меняются местами.
 * 0 (по умолчанию) — модуль установлен без наклона, код не компилируется. */
#ifndef PAJ_GESTURE_ROTATE
#define PAJ_GESTURE_ROTATE              0
#endif

/* ---------------------------------------------------------------------------
 * Пороги proximity (REG_PS_TH_HIGH 0x69 / REG_PS_TH_LOW 0x6A, банк 0)
 *
 * Значения по умолчанию — datasheet (0xC8 / 0x40). Используются, пока
 * калибровка не выполнилась (или отключена через PAJ_ENABLE_CALIBRATION=0).
 * ------------------------------------------------------------------------- */
#ifndef PAJ_PROX_THRESHOLD
#define PAJ_PROX_THRESHOLD              200   /* 0xC8 */
#endif
#ifndef PAJ_PROX_EXIT_TH
#define PAJ_PROX_EXIT_TH                64    /* 0x40 */
#endif

/* ---------------------------------------------------------------------------
 * Калибровка порогов proximity
 *
 * Драйвер снимает PAJ_CAL_SAMPLES замеров REG_PS_DATA, сортирует их,
 * берёт медиану и добавляет PAJ_CAL_SIGMA_COEFF * sigma. Результат
 * сохраняется для последующих переинициализаций датчика.
 * ------------------------------------------------------------------------- */
#ifndef PAJ_ENABLE_CALIBRATION
#define PAJ_ENABLE_CALIBRATION          1
#endif
#ifndef PAJ_CAL_SAMPLES
#define PAJ_CAL_SAMPLES                 16
#endif
#ifndef PAJ_CAL_SIGMA_COEFF
#define PAJ_CAL_SIGMA_COEFF             3
#endif
#ifndef PAJ_CAL_PROX_MIN
#define PAJ_CAL_PROX_MIN                8
#endif
#ifndef PAJ_CAL_PROX_MAX
#define PAJ_CAL_PROX_MAX                250
#endif
#ifndef PAJ_CAL_FILTER_MAX
#define PAJ_CAL_FILTER_MAX              250
#endif
/* Пауза между замерами при калибровке, мс. */
#ifndef PAJ_CAL_INTERVAL_MS
#define PAJ_CAL_INTERVAL_MS             2
#endif

/* ---------------------------------------------------------------------------
 * Тайминги
 * ------------------------------------------------------------------------- */

/* Таймаут ожидания готовности датчика после I2C wake-up, мс.
 * Datasheet: T1 > 700 мкс (стабилизация после питания), T2 > 400 мкс
 * (после wake-up до первого чтения). Значение по умолчанию — с большим запасом,
 * покрывает и холодный старт, и выход из suspend. */
#ifndef PAJ_WAKEUP_TIMEOUT_MS
#define PAJ_WAKEUP_TIMEOUT_MS           20
#endif

/* Максимальное время блокирующего ожидания жеста в paj_readGesture(), мс. */
#ifndef PAJ_GESTURE_TIMEOUT_MS
#define PAJ_GESTURE_TIMEOUT_MS          300
#endif

/* 1 (по умолчанию): на время paj_readGesture() драйвер конфигурирует SysTick
 * как свободнобегущий счётчик (HCLK/8) и обеспечивает настоящий дедлайн в мс.
 * 0: драйвер не трогает SysTick вовсе — для приложений, использующих его
 * иначе (свой SysTick_Handler и т.п.). Цикл ограничивается бюджетом итераций,
 * каждая итерация содержит poll_delay(), поэтому таймаут не менее заданного. */
#ifndef PAJ_OWN_SYSTICK
#define PAJ_OWN_SYSTICK                 1
#endif

/* Повторная инициализация после ошибок I2C. */
#ifndef PAJ_RETRY_LIMIT
#define PAJ_RETRY_LIMIT                 6
#endif

/* ---------------------------------------------------------------------------
 * Режимы работы
 * ------------------------------------------------------------------------- */

/* 0 = polling, 1 = INT/GVALID interrupt. Определяет стартовую маску
 * прерываний, записываемую paj_init(); переключается в рантайме через
 * paj_enableInterrupt() / paj_disableInterrupt(). */
#ifndef PAJ_INT_MODE
#define PAJ_INT_MODE                    1
#endif

/* 1 = paj_int_config.c экспортирует стандартный strong EXTI7_0_IRQHandler.
 * 0 = приложение само определяет обработчик и вызывает
 * paj_handle_exti() / paj_clear_exti() для линии INT датчика. */
#ifndef PAJ_PROVIDE_EXTI_ISR
#define PAJ_PROVIDE_EXTI_ISR            1
#endif

/* 1 (по умолчанию): драйвер предоставляет paj_setProximityMode() /
 * paj_setGestureMode() — официальные последовательности переключения режимов
 * (datasheet "iii. Change to PS mode" / "v. Change to Gesture mode").
 * Нужны только для работы с proximity: в режиме жестов регистры PS не
 * обновляются. 0 = функции не компилируются (экономия ~250 байт флэша). */
#ifndef PAJ_ENABLE_PS_MODE
#define PAJ_ENABLE_PS_MODE              1
#endif

/* ---------------------------------------------------------------------------
 * Проверки диапазонов: ошибочная конфигурация останавливает сборку.
 * ------------------------------------------------------------------------- */
#if (PAJ_GESTURE_MASK < 0) || (PAJ_GESTURE_MASK > 0xFF)
#error "PAJ_GESTURE_MASK must be in range 0x00..0xFF"
#endif
#if (PAJ_WAVE_MASK != 0) && (PAJ_WAVE_MASK != 1)
#error "PAJ_WAVE_MASK must be 0 or 1"
#endif
#if (PAJ_PS_MASK != 0) && (PAJ_PS_MASK != 1)
#error "PAJ_PS_MASK must be 0 or 1"
#endif
#if (PAJ_GESTURE_ROTATE < 0) || (PAJ_GESTURE_ROTATE > 3)
#error "PAJ_GESTURE_ROTATE must be in range 0..3"
#endif
#if (PAJ_PROX_THRESHOLD < 1) || (PAJ_PROX_THRESHOLD > 255)
#error "PAJ_PROX_THRESHOLD must be in range 1..255"
#endif
#if (PAJ_PROX_EXIT_TH < 0) || (PAJ_PROX_EXIT_TH >= PAJ_PROX_THRESHOLD)
#error "PAJ_PROX_EXIT_TH must be in range 0..PAJ_PROX_THRESHOLD-1"
#endif
#if (PAJ_ENABLE_CALIBRATION != 0) && (PAJ_ENABLE_CALIBRATION != 1)
#error "PAJ_ENABLE_CALIBRATION must be 0 or 1"
#endif
#if (PAJ_CAL_SAMPLES < 4) || (PAJ_CAL_SAMPLES > 32)
#error "PAJ_CAL_SAMPLES must be in range 4..32"
#endif
#if (PAJ_CAL_SIGMA_COEFF < 1) || (PAJ_CAL_SIGMA_COEFF > 10)
#error "PAJ_CAL_SIGMA_COEFF must be in range 1..10"
#endif
#if (PAJ_CAL_PROX_MIN < 1) || (PAJ_CAL_PROX_MIN > PAJ_CAL_PROX_MAX) || (PAJ_CAL_PROX_MAX > 255)
#error "PAJ_CAL_PROX_MIN/MAX must satisfy 1 <= min <= max <= 255"
#endif
#if (PAJ_CAL_FILTER_MAX < 1) || (PAJ_CAL_FILTER_MAX > 255)
#error "PAJ_CAL_FILTER_MAX must be in range 1..255"
#endif
#if (PAJ_CAL_INTERVAL_MS < 1) || (PAJ_CAL_INTERVAL_MS > 100)
#error "PAJ_CAL_INTERVAL_MS must be in range 1..100"
#endif
#if (PAJ_WAKEUP_TIMEOUT_MS < 1) || (PAJ_WAKEUP_TIMEOUT_MS > 1000)
#error "PAJ_WAKEUP_TIMEOUT_MS must be in range 1..1000"
#endif
#if (PAJ_GESTURE_TIMEOUT_MS < 1) || (PAJ_GESTURE_TIMEOUT_MS > 1000)
#error "PAJ_GESTURE_TIMEOUT_MS must be in range 1..1000"
#endif
#if (PAJ_OWN_SYSTICK != 0) && (PAJ_OWN_SYSTICK != 1)
#error "PAJ_OWN_SYSTICK must be 0 or 1"
#endif
#if (PAJ_RETRY_LIMIT < 1) || (PAJ_RETRY_LIMIT > 255)
#error "PAJ_RETRY_LIMIT must be in range 1..255"
#endif
#if (PAJ_INT_MODE != 0) && (PAJ_INT_MODE != 1)
#error "PAJ_INT_MODE must be 0 or 1"
#endif
#if (PAJ_PROVIDE_EXTI_ISR != 0) && (PAJ_PROVIDE_EXTI_ISR != 1)
#error "PAJ_PROVIDE_EXTI_ISR must be 0 or 1"
#endif
#if (PAJ_ENABLE_PS_MODE != 0) && (PAJ_ENABLE_PS_MODE != 1)
#error "PAJ_ENABLE_PS_MODE must be 0 or 1"
#endif

#endif /* PAJ7620_CONFIG_H */
