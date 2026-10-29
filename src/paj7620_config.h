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
 * Маски активных жестов
 *
 * Маска действует на двух уровнях:
 *   1) аппаратно — REG_INT1_EN (0x41) / REG_INT2_EN (0x42): только вывод INT;
 *   2) программно — драйвер вырезает отключённые биты из прочитанных
 *      0x43/0x44 (read_gesture_flags()), поэтому отключённый жест не
 *      участвует ни в декодировании, ни в проверке противоречий, ни в
 *      счётчике paj_getAmbiguousCount(). Без шага 2 отключение было
 *      фиктивным: чип выставляет флаги всегда, и, например, паразитная пара
 *      Forward|Backward от широкой ладони отбрасывала валидный Left.
 *
 * Готовые наборы (складываются из битов GES_* в paj7620_regs.h):
 *   PAJ_GESMASK_DIRECTIONS  0x0F — UP|DOWN|LEFT|RIGHT (4 направления);
 *   PAJ_GESMASK_SIX_AXIS    0x3F — то же + FORWARD|BACKWARD (6 осей);
 *   PAJ_GESMASK_ALL         0xFF — все 8 жестов, включая CW|CCW.
 * Маску можно менять в рантайме: paj_setGestureMask().
 * ------------------------------------------------------------------------- */

#define PAJ_GESMASK_DIRECTIONS          0x0F
#define PAJ_GESMASK_SIX_AXIS            0x3F
#define PAJ_GESMASK_ALL                 0xFF

/* Маска жестов: биты [7:0] = CCW, CW, backward, forward, right, left, down, up
 * (см. GES_* в paj7620_regs.h). 0xFF — реагировать на все 8 жестов. */
#ifndef PAJ_GESTURE_MASK
#define PAJ_GESTURE_MASK                PAJ_GESMASK_ALL
#endif

/* Прерывание по жесту "взмах рукой" (REG_INT2_EN бит 0): 0/1.
 * 0 отключает жест полностью (и программно тоже) и позволяет драйверу не
 * читать второй байт флагов 0x44, когда PAJ_PS_MASK=0: одна транзакция вместо
 * burst-чтения двух регистров. */
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
 * физическое направление жеста. Направление круга (CW/CCW) не
 * переназначается: ориентация обхода инвариантна к повороту модуля в его
 * плоскости; инверсия CW/CCW требуется только при зеркальном монтаже.
 * 0 (по умолчанию) — модуль установлен без наклона, код не компилируется. */
#ifndef PAJ_GESTURE_ROTATE
#define PAJ_GESTURE_ROTATE              0
#endif

/* 1 (по умолчанию): флаги с обеими частями антагонистической пары
 * (Up+Down, Left+Right, Forward+Backward, CW+CCW) означают неуверенную
 * классификацию датчика. Пары чистятся ПООТДЕЛЬНОСТИ: противоречивые биты
 * сбрасываются, остальные сохраняются, поэтому LEFT|FORWARD|BACKWARD
 * по-прежнему декодируется как LEFT (раньше отбрасывалось всё чтение).
 * На аппаратуре наблюдалось 0x43=0x3F; выбор младшего бита давал уверенно
 * неверный жест.
 * 0: разрешить декодирование такого набора с приоритетом драйвера. */
#ifndef PAJ_REJECT_AMBIGUOUS
#define PAJ_REJECT_AMBIGUOUS            1
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
 * Пороги gesture engine по осям (банк 0)
 *
 * 0x93 R_XDirectionThd[4:0] — влево/вправо;
 * 0x94 R_YDirectionThd[4:0] — вверх/вниз;
 * 0x48 R_ZDirectionThd      — push/pull (Z).
 * 0x93/0x94 отсутствуют в официальной initial-таблице, а 0x48 в ней есть с
 * жёстким значением datasheet, поэтому до появления этих макросов порог Z
 * настраивать было нельзя. Значения по умолчанию — заводские/даташитные для
 * проверенного модуля. Меньше — чувствительнее, больше — устойчивее к шуму,
 * но медленные жесты теряются.
 * ------------------------------------------------------------------------- */
#ifndef PAJ_XDIR_THRESHOLD
#define PAJ_XDIR_THRESHOLD              0x0D
#endif
#ifndef PAJ_YDIR_THRESHOLD
#define PAJ_YDIR_THRESHOLD              0x0A
#endif

/* Порог Z-оси (0x48). Широкая ладонь при проходе параллельно датчику даёт
 * резкий скачок размера/яркости объекта и ложный Forward/Backward. Если это
 * происходит — поднимайте значение до 0x48..0x55 (72…85): жест «толчок»
 * станет требовать более выраженного изменения размера объекта. Значение
 * по умолчанию — из начальной последовательности datasheet. */
#ifndef PAJ_ZDIR_THRESHOLD
#define PAJ_ZDIR_THRESHOLD              0x3C
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
/* 0 (по умолчанию): paj_init() не калибрует proximity. В gesture mode
 * REG_PS_DATA зависит от засветки и не отражает приближение; калибруйте
 * явно через paj_setProximityMode() + paj_recalibrate().
 * 1: прежнее поведение с калибровкой в paj_init(). */
#ifndef PAJ_CALIBRATE_ON_INIT
#define PAJ_CALIBRATE_ON_INIT           0
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
 * Неблокирующий опрос и программная фильтрация жестов (paj_poll)
 *
 * Метку времени даёт приложение: драйвер не захватывает SysTick/TIMx и не
 * конфликтует с Delay_Ms(). Все интервалы должны быть меньше 2^31 мс.
 * ------------------------------------------------------------------------- */
#ifndef PAJ_POLL_INTERVAL_MS
#define PAJ_POLL_INTERVAL_MS            5     /* 200 Гц при 120 Гц у датчика */
#endif
/* Окно подтверждения бокового жеста (Up/Down/Left/Right).
 * 20 мс = 4 опроса при PAJ_POLL_INTERVAL_MS=5. Прежние 40 мс были слишком
 * велики для ладони: за это время её середина проходит над датчиком и даёт
 * паразитный Z-флаг. */
#ifndef PAJ_GESTURE_CONFIRM_MS
#define PAJ_GESTURE_CONFIRM_MS          20
#endif
/* Подавление повторных событий после выдачи жеста.
 * Широкая ладонь проходит над датчиком ~180…200 мс (палец — быстрее),
 * поэтому при 150 мс задний край ладони успевал дать ложное повторное
 * срабатывание на выходе. */
#ifndef PAJ_GESTURE_COOLDOWN_MS
#define PAJ_GESTURE_COOLDOWN_MS         230
#endif

/* Политика приоритета Z-оси (Forward/Backward) относительно начатого
 * бокового жеста. 0 (по умолчанию) — режим для управления ладонью:
 *   - одиночный Z-флаг, пришедший пока ждёт подтверждения боковой жест,
 *     считается засветом от широкой ладони и НЕ отменяет кандидата;
 *   - если в одном чтении есть и боковой, и Z-бит, Z-биты отбрасываются;
 *   - если датчик перестал выдавать боковой флаг (GESTURE_NONE), кандидат
 *     подтверждается сразу, не дожидаясь окна PAJ_GESTURE_CONFIRM_MS, —
 *     раньше, чем придёт паразитный Z-флаг с заднего края ладони.
 * 1 — прежнее поведение: одиночный Forward/Backward выдаётся немедленно и
 * отменяет ожидающий боковой жест. Выбирайте 1 только если приложение
 * управляется пальцем и намеренный push/pull важнее устойчивости к ладони. */
#ifndef PAJ_Z_OVERRIDES_XY
#define PAJ_Z_OVERRIDES_XY              0
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

/* Число подряд неудачных чтений до полной переинициализации. Одиночный NACK
 * не должен стирать конфигурацию: wake-up и 47 записей занимают время, в
 * течение которого жесты теряются. В legacy API порог управляет re-init;
 * paj_poll() не переинициализирует скрыто, но использует тот же гистерезис
 * для диагностики отказа. */
#ifndef PAJ_REINIT_THRESHOLD
#define PAJ_REINIT_THRESHOLD            3
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
#if (PAJ_REJECT_AMBIGUOUS != 0) && (PAJ_REJECT_AMBIGUOUS != 1)
#error "PAJ_REJECT_AMBIGUOUS must be 0 or 1"
#endif
#if (PAJ_PROX_THRESHOLD < 1) || (PAJ_PROX_THRESHOLD > 255)
#error "PAJ_PROX_THRESHOLD must be in range 1..255"
#endif
#if (PAJ_PROX_EXIT_TH < 0) || (PAJ_PROX_EXIT_TH >= PAJ_PROX_THRESHOLD)
#error "PAJ_PROX_EXIT_TH must be in range 0..PAJ_PROX_THRESHOLD-1"
#endif
#if (PAJ_XDIR_THRESHOLD < 0) || (PAJ_XDIR_THRESHOLD > 0x1F)
#error "PAJ_XDIR_THRESHOLD must be in range 0..0x1F"
#endif
#if (PAJ_YDIR_THRESHOLD < 0) || (PAJ_YDIR_THRESHOLD > 0x1F)
#error "PAJ_YDIR_THRESHOLD must be in range 0..0x1F"
#endif
#if (PAJ_ZDIR_THRESHOLD < 0) || (PAJ_ZDIR_THRESHOLD > 0xFF)
#error "PAJ_ZDIR_THRESHOLD must be in range 0x00..0xFF"
#endif
#if (PAJ_Z_OVERRIDES_XY != 0) && (PAJ_Z_OVERRIDES_XY != 1)
#error "PAJ_Z_OVERRIDES_XY must be 0 or 1"
#endif
#if (PAJ_ENABLE_CALIBRATION != 0) && (PAJ_ENABLE_CALIBRATION != 1)
#error "PAJ_ENABLE_CALIBRATION must be 0 or 1"
#endif
#if (PAJ_CALIBRATE_ON_INIT != 0) && (PAJ_CALIBRATE_ON_INIT != 1)
#error "PAJ_CALIBRATE_ON_INIT must be 0 or 1"
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
#if (PAJ_REINIT_THRESHOLD < 1) || (PAJ_REINIT_THRESHOLD > 255)
#error "PAJ_REINIT_THRESHOLD must be in range 1..255"
#endif
#if (PAJ_POLL_INTERVAL_MS < 1) || (PAJ_POLL_INTERVAL_MS > 100)
#error "PAJ_POLL_INTERVAL_MS must be in range 1..100"
#endif
#if (PAJ_GESTURE_CONFIRM_MS < 0) || (PAJ_GESTURE_CONFIRM_MS > 1000)
#error "PAJ_GESTURE_CONFIRM_MS must be in range 0..1000"
#endif
#if (PAJ_GESTURE_COOLDOWN_MS < 0) || (PAJ_GESTURE_COOLDOWN_MS > 2000)
#error "PAJ_GESTURE_COOLDOWN_MS must be in range 0..2000"
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
