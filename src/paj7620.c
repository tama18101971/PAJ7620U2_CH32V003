/*
 * paj7620.c — Драйвер распознавания жестов для PAJ7620U2.
 *
 * Жесты распознаёт аппаратный gesture engine самого датчика: он формирует
 * битовые флаги в REG_INT_FLAG1/REG_INT_FLAG2, а драйвер читает и декодирует
 * их. Никакой обработки пиксельных данных и никакой плавающей точки.
 *
 * Все последовательности регистров взяты из datasheet
 * (docs/PAJ7620U2_DS.pdf, "Programing Sequence and Function Application"):
 *   - инициализация      — "i. Initial"
 *   - режим proximity    — "iii. Change to PS mode"
 *   - режим жестов       — "v. Change to Gesture mode"
 *   - suspend / resume   — "vi. Enter Suspend mode" / "vii. Resume Gesture"
 *
 * Потребление RAM: ~32 байта статики (+ буфер калибровки в стеке).
 */

#include "paj7620.h"
#include "paj7620_regs.h"
#include "i2c.h"
#include <stdint.h>
#include <stdbool.h>
#include <debug.h>

#ifdef PAJ7620U2_DEBUG
/* printf используется только в отладочных ветках ниже; без PAJ7620U2_DEBUG
 * заголовок не нужен и роняет -Wunused-подобные проверки clangd. */
#include <stdio.h>
#endif

/* Количество элементов статического массива (uint8_t достаточно: таблицы
 * конфигурации короче 255 записей). */
#define ARRAY_LEN(a)    ((uint8_t)(sizeof(a) / sizeof((a)[0])))

/* ============================================================================
 * СОСТОЯНИЕ ДРАЙВЕРА
 * ============================================================================ */

/* Сырой статус последней НЕудачной I2C-транзакции (коды i2c.h:
 * I2C_OK=0, I2C_NACK=1, I2C_ERR_TIMEOUT=2, I2C_ERR_CLK=3,
 * I2C_ERR_BERR=4, I2C_ERR_ARLO=5). Обновляется только при сбое,
 * успех статус не сбрасывает. Доступен через paj_getLastI2CStatus(). */
static uint8_t g_last_i2c_status;

/* g_last_error — публичный код последней ошибки (PAJ_ERR_*). */
static uint8_t g_last_error;

/* g_reinit_count — счётчик последовательных переинициализаций после сбоев
 * I2C. Сбрасывается после любого успешного чтения датчика. */
static uint8_t g_reinit_count;

/* Число неудачных чтений подряд. Полная переинициализация в legacy API
 * выполняется только после PAJ_REINIT_THRESHOLD сбоев: одиночный NACK не
 * должен стирать конфигурацию датчика. Сбрасывается успешным чтением. */
static uint8_t g_fail_streak;

/* Текущий банк регистров. PAJ_BANK_UNKNOWN означает «состояние неизвестно»
 * (после I2C wake-up/пробуждения датчика) — банк будет выбран принудительно. */
#define PAJ_BANK_UNKNOWN   0xFF
static uint8_t g_bank = PAJ_BANK_UNKNOWN;

/* Кэш жеста, прочитанного paj_available()/paj_clearInterrupt(): датчик
 * сбрасывает флаги при чтении, поэтому жест переживает чтение только здесь. */
static gesture_t g_gesture;
static uint8_t   g_has_gesture;

/* Состояние неблокирующего фильтра paj_poll(). Метки uint32_t сравниваются
 * знаковой разностью, поэтому переполнение счётчика миллисекунд безопасно. */
static uint32_t g_next_poll_ms;
static uint32_t g_pending_deadline_ms;
static uint32_t g_cooldown_until_ms;
static uint8_t  g_pending_gesture;

/* Откалиброванные пороги proximity и признак их действительности. */
static uint8_t  g_cal_hi;
static uint8_t  g_cal_lo;
static uint8_t  g_cal_valid;

/* Число чтений с антагонистической парой флагов: датчик не выбрал
 * направление (например, одновременно Up и Down). Диагностика качества
 * сигнала через paj_getAmbiguousCount(). */
static uint8_t  g_ambiguous;

/* ============================================================================
 * ОБЕРТКИ НАД I2C (адрес датчика 0x73, два банка регистров)
 * ============================================================================ */

/* Сохранить код неудачной транзакции для диагностики. */
static void note_i2c(uint8_t st) {
    g_last_i2c_status = st;
}

/* Выбор банка регистров. Повторная запись не выполняется, если датчик уже
 * в нужном банке, — это убирает лишние транзакции из горячего пути опроса
 * и из длинных последовательностей инициализации.
 * Возвращает: true если банк выбран. */
static bool bank_sel(uint8_t bank) {
    if (g_bank == bank) {
        return true;
    }
    uint8_t st = i2c_write_register(PAJ7620_I2C_ADDR, REG_BANK_SEL, bank);
    if (st != I2C_OK) {
        note_i2c(st);
        return false;
    }
    g_bank = bank;
    return true;
}

/* Чтение одного регистра датчика из указанного банка.
 * bank — PAJ_B0/PAJ_B1, reg — адрес регистра, out — указатель для значения.
 * Возвращает: true если I2C успешен. При сбое сохраняет код i2c.h
 * в g_last_i2c_status. */
static bool rd(uint8_t bank, uint8_t reg, uint8_t *out) {
    if (!bank_sel(bank)) {
        return false;
    }
    uint8_t st = i2c_read_register(PAJ7620_I2C_ADDR, reg, out);
    if (st != I2C_OK) {
        note_i2c(st);
        return false;
    }
    return true;
}

/* Запись одного регистра датчика в указанном банке.
 * reg — адрес регистра, val — записываемое значение.
 * Возвращает: true если I2C успешен. При сбое сохраняет код i2c.h
 * в g_last_i2c_status. */
static bool wr(uint8_t bank, uint8_t reg, uint8_t val) {
    if (!bank_sel(bank)) {
        return false;
    }
    uint8_t st = i2c_write_register(PAJ7620_I2C_ADDR, reg, val);
    if (st != I2C_OK) {
        note_i2c(st);
        return false;
    }
    return true;
}

/* Последовательная запись таблицы {bank, reg, value}.
 * Возвращает: true если все записи успешны (останавливается на первой ошибке). */
static bool write_seq(const uint8_t (*seq)[3], uint8_t count) {
    for (uint8_t i = 0; i < count; i++) {
        if (!wr(seq[i][0], seq[i][1], seq[i][2])) {
            return false;
        }
    }
    return true;
}

/* Беззнаковое деление через сдвиги и вычитание. На RV32EC нет аппаратного
 * деления; этот небольшой вариант не вызывает __udivsi3 из libgcc.
 * Используется только в init/legacy timeout и калибровке, не в paj_poll().
 * Делитель обязан быть ненулевым. */
#if PAJ_OWN_SYSTICK || PAJ_ENABLE_CALIBRATION
static uint32_t udiv32_u16(uint32_t numerator, uint16_t denominator) {
    uint32_t quotient = 0;
    uint32_t remainder = 0;
    for (uint8_t bit = 32; bit-- > 0;) {
        remainder = (remainder << 1) | ((numerator >> bit) & 1UL);
        if (remainder >= denominator) {
            remainder -= denominator;
            quotient |= 1UL << bit;
        }
    }
    return quotient;
}
#endif

/* ============================================================================
 * ТАЙМБЕЙС ЦИКЛА paj_readGesture()
 *
 * ВАЖНО: SysTick в NoneOS-SDK НЕ свободнобегущий. Delay_Ms()/Delay_Us()
 * (Debug/ch32v00x/debug.c) каждый раз пишут CMP, ОБНУЛЯЮТ CNT, запускают
 * счёт и по завершении ОСТАНАВЛИВАЮТ его (CTLR bit0 -> 0). Между вызовами
 * Delay_* счётчик стоит на месте. Поэтому читать SysTick->CNT как «текущее
 * время» при активном использовании Delay_* бессмысленно.
 *
 * PAJ_OWN_SYSTICK=1: драйвер на время цикла запускает SysTick сам
 * (CNT=0, CMP=max, счёт вверх от нуля, без прерывания) и внутри цикла
 * Delay_* НЕ вызывает — пауза между опросами флагов считается по CNT.
 * Тактирование HCLK/8 (STCLK=0 после сброса) — та же посылка, что в
 * Delay_Init(): p_us = SystemCoreClock / 8000000.
 *
 * PAJ_OWN_SYSTICK=0: SysTick не трогаем, цикл ограничен бюджетом итераций,
 * каждая из которых занимает не менее 1 мс (poll_delay() -> Delay_Ms(1)) —
 * это нижняя оценка, защищающая от вечного ожидания при залипшем датчике.
 * ============================================================================ */

#if PAJ_OWN_SYSTICK
/* Тиков SysTick на 1 мс; вычисляется один раз в timebase_start(). */
static uint16_t g_ticks_per_ms;

/* Запустить свободнобегущий SysTick от нуля. */
static void timebase_start(void) {
    g_ticks_per_ms = (uint16_t)udiv32_u16(SystemCoreClock >> 3, 1000); /* HCLK/8 */
    SysTick->SR   = 0;
    SysTick->CMP  = 0xFFFFFFFFu;
    SysTick->CNT  = 0;
    SysTick->CTLR |= 1u;              /* STE=1: счёт вверх от нуля */
}

/* Подождать перехода к следующему опросу флагов.
 * Требует запущенного timebase_start(); SysTick не трогает. */
static void poll_delay(void) {
    uint32_t until = SysTick->CNT + g_ticks_per_ms;
    while ((int32_t)(until - SysTick->CNT) > 0) {}
}

/* true, если дедлайн (мс от timebase_start()) истёк. */
static bool timebase_expired(uint16_t deadline_ms) {
    return SysTick->CNT >= (uint32_t)g_ticks_per_ms * deadline_ms;
}
#else
/* Пауза 1 мс штатным средством SDK (SysTick он перенастроит сам). */
static void poll_delay(void) {
    Delay_Ms(1);
}
#endif

/* ============================================================================
 * ТАБЛИЦЫ РЕГИСТРОВ (datasheet V0.8, "Programing Sequence")
 *
 * Формат записи: {номер банка, адрес регистра, значение}.
 * Отдельные записи {0xEF, банк} опущены — их выполняет bank_sel().
 * Запись Enable (bank1 0x72 = 0x01) вынесена из таблицы инициализации:
 * её использует также paj_wakeup() (восстановление из suspend).
 * ============================================================================ */

/* Начальная конфигурация (datasheet "i. Initial") — 47 записей.
 * static const uint8_t[][3]: только .rodata/.text во Flash, без RAM-копии. */
static const uint8_t g_init_seq[][3] = {
    /* банк 0: параметры gesture engine, AE и светового окружения */
    { PAJ_B0, 0x37, 0x07 }, { PAJ_B0, 0x38, 0x17 }, { PAJ_B0, 0x39, 0x06 },
    { PAJ_B0, 0x42, 0x01 }, { PAJ_B0, 0x46, 0x2D }, { PAJ_B0, 0x47, 0x0F },
    { PAJ_B0, 0x48, 0x3C }, { PAJ_B0, 0x49, 0x00 }, { PAJ_B0, 0x4A, 0x1E },
    { PAJ_B0, 0x4C, 0x20 }, { PAJ_B0, 0x51, 0x10 }, { PAJ_B0, 0x5E, 0x10 },
    { PAJ_B0, 0x60, 0x27 }, { PAJ_B0, 0x80, 0x42 }, { PAJ_B0, 0x81, 0x44 },
    { PAJ_B0, 0x82, 0x04 }, { PAJ_B0, 0x8B, 0x01 }, { PAJ_B0, 0x90, 0x06 },
    { PAJ_B0, 0x95, 0x0A }, { PAJ_B0, 0x96, 0x0C }, { PAJ_B0, 0x97, 0x05 },
    { PAJ_B0, 0x9A, 0x14 }, { PAJ_B0, 0x9C, 0x3F }, { PAJ_B0, 0xA5, 0x19 },
    { PAJ_B0, 0xCC, 0x19 }, { PAJ_B0, 0xCD, 0x0B }, { PAJ_B0, 0xCE, 0x13 },
    { PAJ_B0, 0xCF, 0x64 }, { PAJ_B0, 0xD0, 0x21 },
    /* банк 1: AE/AGC, пороги и тайминги автомата состояний */
    { PAJ_B1, 0x02, 0x0F }, { PAJ_B1, 0x03, 0x10 }, { PAJ_B1, 0x04, 0x02 },
    { PAJ_B1, 0x25, 0x01 }, { PAJ_B1, 0x27, 0x39 }, { PAJ_B1, 0x28, 0x7F },
    { PAJ_B1, 0x29, 0x08 }, { PAJ_B1, 0x3E, 0xFF }, { PAJ_B1, 0x5E, 0x3D },
    { PAJ_B1, 0x65, 0x96 }, { PAJ_B1, 0x67, 0x97 }, { PAJ_B1, 0x69, 0xCD },
    { PAJ_B1, 0x6A, 0x01 }, { PAJ_B1, 0x6D, 0x2C }, { PAJ_B1, 0x6E, 0x01 },
    { PAJ_B1, 0x73, 0x35 }, { PAJ_B1, 0x74, 0x00 }, { PAJ_B1, 0x77, 0x01 },
};

#if PAJ_ENABLE_PS_MODE
/* Режим proximity (datasheet "iii. Change to PS mode") — 25 записей. */
static const uint8_t g_ps_seq[][3] = {
    { PAJ_B0, 0x41, 0x00 }, { PAJ_B0, 0x42, 0x02 }, { PAJ_B0, 0x48, 0x20 },
    { PAJ_B0, 0x49, 0x00 }, { PAJ_B0, 0x51, 0x13 }, { PAJ_B0, 0x83, 0x00 },
    { PAJ_B0, 0x9F, 0xF8 }, { PAJ_B0, 0x69, 0x96 }, { PAJ_B0, 0x6A, 0x02 },
    { PAJ_B1, 0x01, 0x1E }, { PAJ_B1, 0x02, 0x0F }, { PAJ_B1, 0x03, 0x10 },
    { PAJ_B1, 0x04, 0x02 }, { PAJ_B1, 0x41, 0x50 }, { PAJ_B1, 0x43, 0x34 },
    { PAJ_B1, 0x65, 0xCE }, { PAJ_B1, 0x66, 0x0B }, { PAJ_B1, 0x67, 0xCE },
    { PAJ_B1, 0x68, 0x0B }, { PAJ_B1, 0x69, 0xE9 }, { PAJ_B1, 0x6A, 0x05 },
    { PAJ_B1, 0x6B, 0x50 }, { PAJ_B1, 0x6C, 0xC3 }, { PAJ_B1, 0x6D, 0x50 },
    { PAJ_B1, 0x6E, 0xC3 }, { PAJ_B1, 0x74, 0x05 },
};

/* Возврат в режим жестов (datasheet "v. Change to Gesture mode") — 24 записи.
 * Последние две (маски прерываний) не включены: их значения задаются
 * конфигурацией и меняются paj_enableInterrupt()/paj_disableInterrupt(). */
static const uint8_t g_gesture_seq[][3] = {
    { PAJ_B0, 0x41, 0x00 }, { PAJ_B0, 0x42, 0x00 }, { PAJ_B0, 0x48, 0x3C },
    { PAJ_B0, 0x49, 0x00 }, { PAJ_B0, 0x51, 0x10 }, { PAJ_B0, 0x83, 0x20 },
    { PAJ_B0, 0x9F, 0xF9 },
    { PAJ_B1, 0x01, 0x1E }, { PAJ_B1, 0x02, 0x0F }, { PAJ_B1, 0x03, 0x10 },
    { PAJ_B1, 0x04, 0x02 }, { PAJ_B1, 0x41, 0x40 }, { PAJ_B1, 0x43, 0x30 },
    { PAJ_B1, 0x65, 0x96 }, { PAJ_B1, 0x66, 0x00 }, { PAJ_B1, 0x67, 0x97 },
    { PAJ_B1, 0x68, 0x01 }, { PAJ_B1, 0x69, 0xCD }, { PAJ_B1, 0x6A, 0x01 },
    { PAJ_B1, 0x6B, 0xB0 }, { PAJ_B1, 0x6C, 0x04 }, { PAJ_B1, 0x6D, 0x2C },
    { PAJ_B1, 0x6E, 0x01 }, { PAJ_B1, 0x74, 0x00 },
};
#endif /* PAJ_ENABLE_PS_MODE */

/* ============================================================================
 * ПРОБУЖДЕНИЕ И ВКЛЮЧЕНИЕ
 * ============================================================================ */

/* Сброс только временного автомата: уже готовый жест при восстановлении I2C
 * сохраняется, а неоднозначный кандидат отбрасывается. */
static void filter_reset(void) {
    g_next_poll_ms         = 0;
    g_pending_deadline_ms  = 0;
    g_cooldown_until_ms    = 0;
    g_pending_gesture      = GESTURE_NONE;
}

/* Полный сброс при init/wakeup/смене режима. */
static void gesture_reset(void) {
    g_gesture     = GESTURE_NONE;
    g_has_gesture = 0;
    filter_reset();
}

/* I2C wake-up: START + slave ID + STOP.
 * Datasheet: "Write slave ID or I2C read command to process I2C wake-up".
 * До окончания пробуждения датчик не отвечает ACK, поэтому результат
 * транзакции здесь не интерпретируется — готовность проверяется отдельно
 * по регистру 0x00. Состояние банка после wake-up неизвестно. */
static void wakeup_cmd(void) {
    uint8_t st = i2c_start();
    if (st == I2C_OK) {
        st = i2c_send_addr(PAJ7620_I2C_ADDR, I2C_DIR_TX);
    }
    if (st == I2C_OK) {
        st = i2c_stop();
    }
    if (st != I2C_OK) {
        note_i2c(st);
    }
    g_bank = PAJ_BANK_UNKNOWN;
}

/* Ожидание готовности датчика после wake-up.
 * Datasheet: T1 > 700 мкс (после питания), T2 > 400 мкс (после wake-up),
 * признак готовности — 0x20 в регистре 0x00 (банк 0).
 * NACK до окончания пробуждения — штатная ситуация и в диагностику не
 * попадает; прочие коды ошибок шины фиксируются.
 * Возвращает: true когда датчик готов. При 0xFF — PAJ_ERR_INVALID_ID,
 * по истечении таймаута — PAJ_ERR_SENSOR_HANG. */
static bool wait_ready(void) {
    for (uint8_t i = 0; i < PAJ_WAKEUP_TIMEOUT_MS; i++) {
        uint8_t v;
        uint8_t st;

        /* Регистр состояния пробуждения лежит в банке 0, а после suspend
         * банк не сохраняется — поэтому он выбирается явно. Пока датчик ещё
         * спит, запись не проходит и банк остаётся неизвестным, что для
         * следующих итераций безопасно: повтор выполняется заново. */
        st = bank_sel(PAJ_B0) ? i2c_read_register(PAJ7620_I2C_ADDR, REG_WAKEUP_STAT, &v)
                              : (uint8_t)I2C_NACK;

        if (st == I2C_OK) {
            if (v == PAJ_WAKEUP_DONE) {
                /* T2 > 400 мкс между wake-up и первым чтением регистров. */
                Delay_Ms(1);
                return true;
            }
            if (v == 0xFF) {
                /* Устройство отвечает, но это не PAJ7620U2. */
                g_last_error = PAJ_ERR_INVALID_ID;
                return false;
            }
        } else if (st != I2C_NACK) {
            note_i2c(st);
        }
        Delay_Ms(1);
    }
    g_last_error = PAJ_ERR_SENSOR_HANG;
    return false;
}

/* Запись порогов proximity: откалиброванные, иначе — из конфигурации. */
static bool apply_thresholds(void) {
    uint8_t hi = g_cal_valid ? g_cal_hi : (uint8_t)PAJ_PROX_THRESHOLD;
    uint8_t lo = g_cal_valid ? g_cal_lo : (uint8_t)PAJ_PROX_EXIT_TH;
    return wr(PAJ_B0, REG_PS_TH_HIGH, hi) && wr(PAJ_B0, REG_PS_TH_LOW, lo);
}

/* Порогов X/Y нет в официальной initial-таблице, поэтому после сброса они
 * остаются заводскими. Явная запись делает их воспроизводимыми и позволяет
 * настраивать чувствительность боковых жестов. */
static bool apply_axis_thresholds(void) {
    return wr(PAJ_B0, REG_GEST_TH_X, (uint8_t)PAJ_XDIR_THRESHOLD) &&
           wr(PAJ_B0, REG_GEST_TH_Y, (uint8_t)PAJ_YDIR_THRESHOLD);
}

/* Запись масок прерываний (REG_INT1_EN / REG_INT2_EN, банк 0). */
static bool write_int_masks(uint8_t m1, uint8_t m2) {
    return wr(PAJ_B0, REG_INT1_EN, m1) && wr(PAJ_B0, REG_INT2_EN, m2);
}

#define PAJ_INT2_MASK_ON  ((uint8_t)((PAJ_WAVE_MASK ? INT2_WAVE_MASK : 0x00) | \
                                     (PAJ_PS_MASK   ? INT2_PS_MASK   : 0x00)))

/* Маски прерываний, применяемые при инициализации датчика. */
static bool apply_int_masks(void) {
#if PAJ_INT_MODE == 1
    return write_int_masks(PAJ_GESTURE_MASK, PAJ_INT2_MASK_ON);
#else
    return write_int_masks(0x00, 0x00);
#endif
}

/* Включение датчика (bank1 0x72 = 0x01) с проверкой чтением обратно. */
static bool enable_sensor(void) {
    if (!wr(PAJ_B1, REG_ENABLE, EN_ENABLE)) {
        return false;
    }
    uint8_t en;
    if (!rd(PAJ_B1, REG_ENABLE, &en)) {
        return false;
    }
    return (en & EN_ENABLE) != 0;
}

/* Полная конфигурация датчика: официальная начальная последовательность,
 * пороги proximity, включение и маски прерываний.
 * Порядок соответствует datasheet: конфигурация пишется при выключенном
 * датчике, Enable — последней записью. */
static bool configure_sensor(void) {
    if (!write_seq(g_init_seq, ARRAY_LEN(g_init_seq))) {
        return false;
    }
    if (!apply_axis_thresholds()) {
        return false;
    }
    if (!apply_thresholds()) {
        return false;
    }
    if (!enable_sensor()) {
        return false;
    }
    return apply_int_masks();
}

/* Переинициализация датчика после сбоя I2C: полный цикл wake-up и
 * конфигурации, включая восстановление откалиброванных порогов. */
static bool sensor_reinit(void) {
    Delay_Ms(1);                 /* T1 > 700 мкс */
    wakeup_cmd();
    if (!wait_ready()) {
        return false;
    }
    return configure_sensor();
}

/* Восстановление после сбоя I2C. Счётчик сохраняется между сбоями и
 * сбрасывается после любого успешного чтения датчика. Провал
 * sensor_reinit() немедленно отражается как PAJ_ERR_SENSOR_HANG и не
 * маскируется под PAJ_ERR_I2C: иначе по публичному API невозможно отличить
 * «сбой чтения» от «сбоя чтения + неудачное восстановление». */
static void recover_sensor(void) {
    g_fail_streak++;
    if (g_fail_streak < PAJ_REINIT_THRESHOLD) {
        return;                 /* одиночный NACK не стирает конфигурацию */
    }
    g_fail_streak = 0;

    /* Не теряем событие, которое приложение ещё не забрало из кэша. */
    filter_reset();

    if (g_reinit_count < PAJ_RETRY_LIMIT) {
        g_reinit_count++;
        if (!sensor_reinit()) {
            g_last_error = PAJ_ERR_SENSOR_HANG;
            return;
        }
    }

    if (g_reinit_count >= PAJ_RETRY_LIMIT) {
        g_last_error = PAJ_ERR_SENSOR_HANG;
    }
}

/* ============================================================================
 * ЧТЕНИЕ И ДЕКОДИРОВАНИЕ ЖЕСТОВ
 * ============================================================================ */

/* Переназначение направлений при повёрнутом модуле.
 * Подстановка выполняется на этапе компиляции: при PAJ_GESTURE_ROTATE=0
 * макросы тождественны и код не меняется. Модуль, повёрнутый на 90° по
 * часовой стрелке, «ось вверх» смотрит вправо, поэтому жест, который датчик
 * называет UP, физически является RIGHT (и так далее по кругу).
 *
 * CW/CCW НЕ переназначаются ни при каком повороте: направление обхода
 * окружности сохраняется при повороте системы координат в плоскости
 * изображения (поворот — невырожденное преобразование с det=+1, оно не
 * меняет ориентацию плоскости). Инверсия CW/CCW нужна только при зеркальном
 * монтаже (датчик смотрит «изнутри»), но зеркало не выражается никаким
 * значением PAJ_GESTURE_ROTATE — такую компоновку правьте вручную. */
#if PAJ_GESTURE_ROTATE == 1
#  define PAJ_DIR_UP        GESTURE_RIGHT
#  define PAJ_DIR_DOWN      GESTURE_LEFT
#  define PAJ_DIR_LEFT      GESTURE_UP
#  define PAJ_DIR_RIGHT     GESTURE_DOWN
#  define PAJ_DIR_CW        GESTURE_CW
#  define PAJ_DIR_CCW       GESTURE_CCW
#elif PAJ_GESTURE_ROTATE == 2
#  define PAJ_DIR_UP        GESTURE_DOWN
#  define PAJ_DIR_DOWN      GESTURE_UP
#  define PAJ_DIR_LEFT      GESTURE_RIGHT
#  define PAJ_DIR_RIGHT     GESTURE_LEFT
#  define PAJ_DIR_CW        GESTURE_CW
#  define PAJ_DIR_CCW       GESTURE_CCW
#elif PAJ_GESTURE_ROTATE == 3
#  define PAJ_DIR_UP        GESTURE_LEFT
#  define PAJ_DIR_DOWN      GESTURE_RIGHT
#  define PAJ_DIR_LEFT      GESTURE_DOWN
#  define PAJ_DIR_RIGHT     GESTURE_UP
#  define PAJ_DIR_CW        GESTURE_CW
#  define PAJ_DIR_CCW       GESTURE_CCW
#else
#  define PAJ_DIR_UP        GESTURE_UP
#  define PAJ_DIR_DOWN      GESTURE_DOWN
#  define PAJ_DIR_LEFT      GESTURE_LEFT
#  define PAJ_DIR_RIGHT     GESTURE_RIGHT
#  define PAJ_DIR_CW        GESTURE_CW
#  define PAJ_DIR_CCW       GESTURE_CCW
#endif

/* Декодирование флагов датчика в тип жеста.
 * Если датчик сообщил составной набор, Z-жест важнее бокового: это устраняет
 * выдачу Up/Down/Left/Right вместо одновременно распознанного push/pull.
 * Датчик не сообщает порядок установки битов, поэтому это явная политика
 * драйвера, а не восстановленная временная последовательность. */
/* Обе части антагонистической пары одновременно означают, что датчик не
 * выбрал направление (на аппаратуре наблюдалось 0x43 = 0x3F). */
static bool flags_contradictory(uint8_t flag1) {
    return ((flag1 & GES_PAIR_UP_DOWN)    == GES_PAIR_UP_DOWN)    ||
           ((flag1 & GES_PAIR_LEFT_RIGHT) == GES_PAIR_LEFT_RIGHT) ||
           ((flag1 & GES_PAIR_FWD_BACK)   == GES_PAIR_FWD_BACK)   ||
           ((flag1 & GES_PAIR_ROTATE)     == GES_PAIR_ROTATE);
}

static gesture_t decode_gesture(uint8_t flag1, uint8_t flag2) {
#if PAJ_REJECT_AMBIGUOUS
    if (flags_contradictory(flag1)) {
        return GESTURE_NONE;
    }
#endif

    gesture_t g;

    if      (flag1 & GES_FORWARD)    g = GESTURE_FORWARD;
    else if (flag1 & GES_BACKWARD)   g = GESTURE_BACKWARD;
    else if (flag1 & GES_CW)         g = PAJ_DIR_CW;
    else if (flag1 & GES_CCW)        g = PAJ_DIR_CCW;
    else if (flag2 & INT2_WAVE_FLAG) g = GESTURE_WAVE;
    else if (flag1 & GES_UP)         g = PAJ_DIR_UP;
    else if (flag1 & GES_DOWN)       g = PAJ_DIR_DOWN;
    else if (flag1 & GES_LEFT)       g = PAJ_DIR_LEFT;
    else if (flag1 & GES_RIGHT)      g = PAJ_DIR_RIGHT;
    else                             g = GESTURE_NONE;

    return g;
}

/* Burst-read 0x43..0x44 за одну I2C-транзакцию (datasheet, Burst Read).
 * В Lite-варианте I2C-библиотеки остаются два чтения; f1 сохраняется даже
 * при ошибке f2, потому что успешное чтение 0x43 уже очистило его флаги. */
static bool read_gesture_flags(uint8_t *f1, uint8_t *f2) {
    *f1 = 0;
    *f2 = 0;
    if (!bank_sel(PAJ_B0)) {
        return false;
    }

    uint8_t st;
#ifndef I2C_DISABLE_BUFFER_API
    uint8_t flags[2] = {0, 0};
    st = i2c_read_buffer(PAJ7620_I2C_ADDR, REG_INT_FLAG1, flags, 2);
    *f1 = flags[0];
    *f2 = flags[1];
#else
    st = i2c_read_register(PAJ7620_I2C_ADDR, REG_INT_FLAG1, f1);
    if (st == I2C_OK) {
        st = i2c_read_register(PAJ7620_I2C_ADDR, REG_INT_FLAG2, f2);
    }
#endif
    if (flags_contradictory(*f1)) {
        g_ambiguous++;
    }
    if (st != I2C_OK) {
        note_i2c(st);
        return false;
    }

    /* Оба байта прочитаны — цепочка сбоев I2C прервана. */
    g_fail_streak = 0;
    return true;
}

/* Сравнение меток времени корректно при переполнении uint32_t, если окно
 * короче 2^31 мс (конфигурация ограничивает окна намного строже). */
static bool time_reached(uint32_t now_ms, uint32_t deadline_ms) {
    return (int32_t)(now_ms - deadline_ms) >= 0;
}

static bool is_directional(gesture_t g) {
    return g == GESTURE_UP || g == GESTURE_DOWN ||
           g == GESTURE_LEFT || g == GESTURE_RIGHT;
}

/* Публикация единственного готового результата и запуск cooldown. */
static void publish_gesture(gesture_t g, uint32_t now_ms) {
    g_gesture           = g;
    g_has_gesture       = 1;
    g_pending_gesture   = GESTURE_NONE;
    g_cooldown_until_ms = now_ms + (uint32_t)PAJ_GESTURE_COOLDOWN_MS;
}

/* Entry-фильтр и cooldown. Вызов с GESTURE_NONE нужен для подтверждения
 * ранее сохранённого бокового жеста по истечении временного окна. */
static void filter_gesture(gesture_t g, uint32_t now_ms) {
    if (!time_reached(now_ms, g_cooldown_until_ms)) {
        g_pending_gesture = GESTURE_NONE;
        return;                       /* флаги уже прочитаны и очищены */
    }

    if (g == GESTURE_FORWARD || g == GESTURE_BACKWARD) {
        publish_gesture(g, now_ms);   /* Z-жест отменяет бокового кандидата */
        return;
    }

    if (is_directional(g)) {
#if PAJ_GESTURE_CONFIRM_MS == 0
        publish_gesture(g, now_ms);
#else
        if (g_pending_gesture != (uint8_t)g) {
            g_pending_gesture     = (uint8_t)g;
            g_pending_deadline_ms = now_ms + (uint32_t)PAJ_GESTURE_CONFIRM_MS;
        } else if (time_reached(now_ms, g_pending_deadline_ms)) {
            publish_gesture(g, now_ms);
        }
#endif
        return;
    }

    if (g != GESTURE_NONE) {
        publish_gesture(g, now_ms);   /* CW/CCW/WAVE не задерживаем */
        return;
    }

#if PAJ_GESTURE_CONFIRM_MS != 0
    if (g_pending_gesture != GESTURE_NONE &&
        time_reached(now_ms, g_pending_deadline_ms)) {
        publish_gesture((gesture_t)g_pending_gesture, now_ms);
    }
#endif
}

/* Совместимый непосредственный опрос для paj_available()/clearInterrupt().
 * Непрочитанный кэш больше не перезаписывается новым событием. */
static bool poll_gesture(void) {
    uint8_t f1, f2;
    bool ok = read_gesture_flags(&f1, &f2);
    gesture_t g = decode_gesture(f1, f2);

    if (g != GESTURE_NONE && !g_has_gesture) {
        g_gesture     = g;
        g_has_gesture = 1;
#ifdef PAJ7620U2_DEBUG
        printf("GES: raw=%02X/%02X -> %d\r\n", f1, f2, (int)g);
#endif
    }
    return ok;
}

/* ============================================================================
 * КАЛИБРОВКА ПОРОГОВ PROXIMITY
 * ============================================================================ */

#if PAJ_ENABLE_CALIBRATION

/* Целочисленный квадратный корень без деления (поразрядный restoring
 * algorithm). В отличие от итерации Ньютона не вызывает __divsi3/__udivsi3.
 * Возвращает floor(√n); используется только при калибровке. */
static uint8_t isqrt(uint16_t n) {
    uint32_t value = n;
    uint32_t root = 0;
    uint32_t bit = 1UL << 14;       /* старшая степень четырёх для uint16_t */

    while (bit > value) {
        bit >>= 2;
    }
    while (bit != 0) {
        if (value >= root + bit) {
            value -= root + bit;
            root = (root >> 1) + bit;
        } else {
            root >>= 1;
        }
        bit >>= 2;
    }
    return (uint8_t)root;
}

/* Сортировка вставками для uint8_t (используется только при калибровке).
 * Индекс j — int16_t, а не int8_t: char-индекс даёт -Wchar-subscripts,
 * а PAJ_CAL_SAMPLES ограничен 32, так что разрядности хватает с запасом. */
static void sort_u8(uint8_t *arr, uint8_t n) {
    for (uint8_t i = 1; i < n; i++) {
        uint8_t  key = arr[i];
        int16_t  j   = (int16_t)i - 1;
        while (j >= 0 && arr[j] > key) {
            arr[j + 1] = arr[j];
            j--;
        }
        arr[j + 1] = key;
    }
}

/* Автоматическая калибровка порогов proximity.
 * Собирает до PAJ_CAL_SAMPLES замеров REG_PS_DATA, отбрасывая нули
 * (датчик не обновляет PS-данные вне режима proximity) и насыщение
 * (значения выше PAJ_CAL_FILTER_MAX), затем ставит пороги на уровне
 * median + PAJ_CAL_SIGMA_COEFF * sigma.
 *
 * @note Преходящая ошибка I2C в середине сбора и недостаток валидных
 * замеров НЕ фатальны: драйвер оставляет пороги из конфигурации и
 * возвращает true — датчик уже сконфигурирован и работоспособен.
 * Ошибкой считается только сбой записи вычисленных порогов: он означает,
 * что конфигурация датчика неполна.
 *
 * Возвращает: true если калибровка выполнена или признана невозможной. */
static bool calibrate_proximity(void) {
    uint8_t buf[PAJ_CAL_SAMPLES];
    uint8_t valid = 0;
    uint8_t total = 0;

    while (valid < PAJ_CAL_SAMPLES && total < (uint8_t)(PAJ_CAL_SAMPLES * 2)) {
        uint8_t v;
        if (!rd(PAJ_B0, REG_PS_DATA, &v)) {
            /* Сбой I2C: сбор прерывается досрочно, пороги остаются дефолтными. */
            break;
        }
        total++;
        if (v != 0 && v <= PAJ_CAL_FILTER_MAX) {
            buf[valid++] = v;
        }
        Delay_Ms(PAJ_CAL_INTERVAL_MS);
    }

    if (valid < (PAJ_CAL_SAMPLES >> 1)) {
        g_cal_valid = 0;
#ifdef PAJ7620U2_DEBUG
        printf("CAL: no PS data (valid=%d), keep cfg thresholds %d/%d\r\n",
               valid, PAJ_PROX_THRESHOLD, PAJ_PROX_EXIT_TH);
#endif
        return true;
    }

    /* Медиана: деление на степень двух заменено сдвигами. */
    sort_u8(buf, valid);
    uint8_t middle = valid >> 1;
    uint8_t median;
    if (valid & 1) {
        median = buf[middle];
    } else {
        median = (uint8_t)(((uint16_t)buf[middle - 1] + buf[middle]) >> 1);
    }

    /* Дисперсия от медианы. Модуль отклонения перед >>2 сохраняет то же
     * округление к нулю, что и прежнее знаковое деление d / 4. */
    uint32_t sum_sq = 0;
    for (uint8_t i = 0; i < valid; i++) {
        uint16_t d = (buf[i] >= median) ? (uint16_t)(buf[i] - median)
                                        : (uint16_t)(median - buf[i]);
        uint16_t d4 = d >> 2;
        sum_sq += (uint16_t)(d4 * d4);
    }
    uint16_t variance = (uint16_t)udiv32_u16(sum_sq, valid);
    /* isqrt(variance) доходит до ~89, поэтому *4 может превысить 255 (до 356)
     * при очень шумном замере. Насыщаем, а не даём uint8_t молча обернуться:
     * завёрнутое значение занижало бы порог и на шумной засветке порог
     * оказывался бы НИЖЕ, чем на чистой. */
    uint16_t sigma_x4 = (uint16_t)isqrt(variance) * 4u;
    uint8_t  sigma    = (sigma_x4 > 255u) ? (uint8_t)255u : (uint8_t)sigma_x4;

    uint16_t entry  = (uint16_t)median + (uint16_t)PAJ_CAL_SIGMA_COEFF * sigma;
    uint16_t exit_th = (entry * 3) >> 2; /* гистерезис: выход ниже входа */

    if (entry > PAJ_CAL_PROX_MAX)      entry = PAJ_CAL_PROX_MAX;
    if (entry < PAJ_CAL_PROX_MIN)      entry = PAJ_CAL_PROX_MIN;
    if (exit_th >= entry)             exit_th = (uint16_t)entry - 1;
    if (exit_th < 1)                  exit_th = 1;

    g_cal_hi    = (uint8_t)entry;
    g_cal_lo    = (uint8_t)exit_th;
    g_cal_valid = 1;

    if (!wr(PAJ_B0, REG_PS_TH_HIGH, g_cal_hi)) {
        return false;
    }
    if (!wr(PAJ_B0, REG_PS_TH_LOW, g_cal_lo)) {
        return false;
    }

#ifdef PAJ7620U2_DEBUG
    printf("CAL: median=%d sigma=%d -> %d/%d\r\n", median, sigma, g_cal_hi, g_cal_lo);
#endif
    return true;
}
#endif /* PAJ_ENABLE_CALIBRATION */

/* ============================================================================
 * ПУБЛИЧНЫЙ API
 * ============================================================================ */

bool paj_init(void) {
    g_bank = PAJ_BANK_UNKNOWN;

    for (uint8_t attempt = 0; attempt < PAJ_RETRY_LIMIT; attempt++) {
        /* T1 > 700 мкс: датчику нужно время на стабилизацию после питания.
         * Пауза одинакова для всех попыток, чтобы повторная инициализация
         * работала и на «тёплом» датчике. */
        Delay_Ms(1);
        wakeup_cmd();
        if (!wait_ready()) {
            /* Ответило не PAJ7620U2 — повтор не поможет, диагностика
             * должна остаться точной (PAJ_ERR_INVALID_ID), а не
             * превратиться в PAJ_ERR_SENSOR_HANG. */
            if (g_last_error == PAJ_ERR_INVALID_ID) {
                return false;
            }
            Delay_Ms(50);
            continue;
        }

        if (configure_sensor()) {
            gesture_reset();
            g_reinit_count = 0;
            g_fail_streak  = 0;
            g_ambiguous    = 0;

#if PAJ_ENABLE_CALIBRATION && PAJ_CALIBRATE_ON_INIT
            if (!calibrate_proximity()) {
                g_last_error = PAJ_ERR_I2C;
                return false;
            }
#elif PAJ_ENABLE_CALIBRATION
            /* В gesture mode REG_PS_DATA не отражает proximity: калибровать
             * можно только явно после paj_setProximityMode(). */
            g_cal_valid = 0;
#endif
            g_last_error = PAJ_ERR_NONE;
            return true;
        }
        Delay_Ms(50);
    }

    g_last_error = PAJ_ERR_SENSOR_HANG;
    return false;
}

bool paj_sleep(void) {
    /* Только снятие Enable: конфигурация регистров сохраняется, датчик
     * не уходит в suspend, поэтому paj_wakeup() возвращает его в работу
     * одной записью. */
    if (!wr(PAJ_B1, REG_ENABLE, 0x00)) {
        g_last_error = PAJ_ERR_I2C;
        return false;
    }
    g_last_error = PAJ_ERR_NONE;
    return true;
}

bool paj_wakeup(void) {
    /* Выход из suspend требует I2C wake-up-команды (datasheet,
     * "vii. Resume Gesture"); для выхода из paj_sleep() она безвредна. */
    g_bank = PAJ_BANK_UNKNOWN;
    Delay_Ms(1);
    wakeup_cmd();
    if (!wait_ready()) {
        /* Код ошибки уже выставлен: PAJ_ERR_INVALID_ID или PAJ_ERR_SENSOR_HANG */
        return false;
    }

    if (!enable_sensor()) {
        g_last_error = PAJ_ERR_I2C;
        return false;
    }
    if (!apply_int_masks()) {
        g_last_error = PAJ_ERR_I2C;
        return false;
    }

    /* Состояние жеста не должно переживать цикл сна: без сброса кэш
     * прерванного сном жеста и счётчик переинициализаций искажают первое
     * чтение после пробуждения. */
    gesture_reset();
    g_reinit_count = 0;
    g_fail_streak  = 0;
    g_ambiguous    = 0;

    g_last_error = PAJ_ERR_NONE;
    return true;
}

bool paj_shutdown(void) {
    /* Datasheet: сначала снять Enable, затем команда suspend. */
    if (!wr(PAJ_B1, REG_ENABLE, 0x00)) {
        g_last_error = PAJ_ERR_I2C;
        return false;
    }
    if (!wr(PAJ_B0, REG_SUSPEND, 0x01)) {
        g_last_error = PAJ_ERR_I2C;
        return false;
    }
    g_bank = PAJ_BANK_UNKNOWN;   /* после suspend состояние банка неизвестно */
    gesture_reset();
    g_last_error = PAJ_ERR_NONE;
    return true;
}

bool paj_poll(uint32_t now_ms) {
    if (g_has_gesture) {
        return true;                  /* не перезаписываем непрочитанный результат */
    }
    if (!time_reached(now_ms, g_next_poll_ms)) {
        return false;                 /* ни I2C, ни активного ожидания */
    }
    g_next_poll_ms = now_ms + (uint32_t)PAJ_POLL_INTERVAL_MS;

    uint8_t f1, f2;
    bool ok = read_gesture_flags(&f1, &f2);
    filter_gesture(decode_gesture(f1, f2), now_ms);

    if (!ok) {
        /* Новый API не скрывает длительную переинициализацию внутри опроса,
         * но одиночный NACK не считается отказом датчика. */
        g_last_error = PAJ_ERR_I2C;
        g_fail_streak++;
        if (g_fail_streak >= PAJ_REINIT_THRESHOLD) {
            g_fail_streak = 0;
            if (g_reinit_count < PAJ_RETRY_LIMIT) {
                g_reinit_count++;
            }
            if (g_reinit_count >= PAJ_RETRY_LIMIT) {
                g_last_error = PAJ_ERR_SENSOR_HANG;
            }
        }
        return g_has_gesture != 0;    /* частично прочитанный f1 не теряем */
    }

    g_reinit_count = 0;
    g_last_error   = PAJ_ERR_NONE;
    return g_has_gesture != 0;
}

gesture_t paj_takeGesture(void) {
    if (!g_has_gesture) {
        return GESTURE_NONE;
    }
    gesture_t g = g_gesture;
    g_gesture     = GESTURE_NONE;
    g_has_gesture = 0;
    return g;
}

bool paj_available(void) {
    if (g_has_gesture) {
        return true;
    }
    if (!poll_gesture()) {
        g_last_error = PAJ_ERR_I2C;
        recover_sensor();
        /* Жест из успешно прочитанного 0x43 не теряется из-за сбоя 0x44. */
        return g_has_gesture != 0;
    }

    g_reinit_count = 0;
    g_last_error   = PAJ_ERR_NONE;
    return g_has_gesture != 0;
}

gesture_t paj_readGesture(void) {
    g_last_error = PAJ_ERR_NONE;

#if PAJ_OWN_SYSTICK
    timebase_start();
#else
    uint16_t budget = (uint16_t)PAJ_GESTURE_TIMEOUT_MS;
#endif

    for (;;) {
        /* Жест, найденный paj_available() или paj_clearInterrupt(). */
        if (g_has_gesture) {
            g_has_gesture = 0;
            return g_gesture;
        }

#if PAJ_OWN_SYSTICK
        if (timebase_expired((uint16_t)PAJ_GESTURE_TIMEOUT_MS)) {
            break;
        }
#else
        if (budget == 0) {
            break;
        }
        budget--;
#endif

        /* Сбой шины внутри ожидания: частично прочитанные флаги не
         * декодируются, жест был бы недостоверным. Диагностика:
         * paj_getLastError() == PAJ_ERR_I2C (или PAJ_ERR_SENSOR_HANG, если
         * переинициализация не удалась) и paj_getLastI2CStatus(). */
        if (!poll_gesture()) {
            g_last_error = PAJ_ERR_I2C;
            recover_sensor();
            return GESTURE_NONE;
        }
        if (g_has_gesture) {
            g_has_gesture = 0;
            return g_gesture;
        }

        poll_delay();
    }

    return GESTURE_NONE;
}

bool paj_readProximity(uint8_t *value) {
    if (!rd(PAJ_B0, REG_PS_DATA, value)) {
        g_last_error = PAJ_ERR_I2C;
        return false;
    }
    g_last_error = PAJ_ERR_NONE;
    return true;
}

bool paj_readStatus(uint8_t *value) {
    if (!rd(PAJ_B0, REG_STATE, value)) {
        g_last_error = PAJ_ERR_I2C;
        return false;
    }
    g_last_error = PAJ_ERR_NONE;
    return true;
}

uint8_t paj_getLastError(void) {
    return g_last_error;
}

uint8_t paj_getLastI2CStatus(void) {
    return g_last_i2c_status;
}

uint8_t paj_getReinitCount(void) {
    return g_reinit_count;
}

uint8_t paj_getAmbiguousCount(void) {
    return g_ambiguous;
}

bool paj_recalibrate(void) {
#if PAJ_ENABLE_CALIBRATION
    if (!calibrate_proximity()) {
        g_last_error = PAJ_ERR_I2C;
        return false;
    }
    g_last_error = PAJ_ERR_NONE;
    return true;
#else
    /* Калибровка скомпилирована наружу: честно отказываем, а не
     * изображаем успех — вызывающий иначе не узнает, что ничего
     * не произошло. */
    g_last_error = PAJ_ERR_UNSUPPORTED;
    return false;
#endif
}

/* ============================================================================
 * ПРЕРЫВАНИЯ
 *
 * Функции не имеют возврата (void-API), но при сбое транзакции фиксируют
 * g_last_error = PAJ_ERR_I2C (сырой код доступен через paj_getLastI2CStatus()).
 * При успехе g_last_error НЕ перезаписывается.
 * ============================================================================ */

void paj_enableInterrupt(void) {
    if (!write_int_masks(PAJ_GESTURE_MASK, PAJ_INT2_MASK_ON)) {
        g_last_error = PAJ_ERR_I2C;
    }
}

void paj_disableInterrupt(void) {
    if (!write_int_masks(0x00, 0x00)) {
        g_last_error = PAJ_ERR_I2C;
    }
}

void paj_clearInterrupt(void) {
    /* Чтение 0x43/0x44 снимает флаги — это и есть сброс прерывания.
     * Обнаруженный при этом жест сохраняется в кэш и не теряется. */
    if (!poll_gesture()) {
        g_last_error = PAJ_ERR_I2C;
    }
}

/* ============================================================================
 * РЕЖИМЫ PROXIMITY / ЖЕСТОВ
 * ============================================================================ */

#if PAJ_ENABLE_PS_MODE

bool paj_setProximityMode(void) {
    /* Переключение шаблона датчика обесценивает кэш жеста и состояние
     * автомата датчика, поэтому состояние сбрасывается. */
    gesture_reset();

    if (!write_seq(g_ps_seq, ARRAY_LEN(g_ps_seq))) {
        g_last_error = PAJ_ERR_I2C;
        return false;
    }
    if (!enable_sensor()) {
        g_last_error = PAJ_ERR_SENSOR_HANG;
        return false;
    }
    if (!apply_int_masks()) {
        g_last_error = PAJ_ERR_I2C;
        return false;
    }
    g_reinit_count = 0;
    g_last_error   = PAJ_ERR_NONE;
    return true;
}

bool paj_setGestureMode(void) {
    gesture_reset();

    if (!write_seq(g_gesture_seq, ARRAY_LEN(g_gesture_seq))) {
        g_last_error = PAJ_ERR_I2C;
        return false;
    }
    if (!apply_thresholds()) {
        g_last_error = PAJ_ERR_I2C;
        return false;
    }
    if (!enable_sensor()) {
        g_last_error = PAJ_ERR_SENSOR_HANG;
        return false;
    }
    if (!apply_int_masks()) {
        g_last_error = PAJ_ERR_I2C;
        return false;
    }
    g_reinit_count = 0;
    g_last_error   = PAJ_ERR_NONE;
    return true;
}

#endif /* PAJ_ENABLE_PS_MODE */
