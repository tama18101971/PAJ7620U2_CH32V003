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
 * Потребление RAM: ~20 байт статика (+ буфер калибровки в стеке).
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

/* g_fail_streak — число неудачных чтений подряд. Полная переинициализация
 * запускается только когда счётчик достигает PAJ_REINIT_THRESHOLD: одиночный
 * NACK (датчик занят автоэкспозицией или шина сбоит в момент транзакции) не
 * должен приводить к перезаписи всей конфигурации датчика — на это время
 * датчик не отвечает и жесты теряются. Обнуляется после успешного чтения и
 * после срабатывания порога. */
static uint8_t g_fail_streak;

/* Текущий банк регистров. PAJ_BANK_UNKNOWN означает «состояние неизвестно»
 * (после I2C wake-up/пробуждения датчика) — банк будет выбран принудительно. */
#define PAJ_BANK_UNKNOWN   0xFF
static uint8_t g_bank = PAJ_BANK_UNKNOWN;

/* Кэш жеста, прочитанного paj_available()/paj_clearInterrupt(): датчик
 * сбрасывает флаги при чтении, поэтому жест переживает чтение только здесь. */
static gesture_t g_gesture;
static uint8_t   g_has_gesture;

/* Откалиброванные пороги proximity и признак их действительности. */
static uint8_t  g_cal_hi;
static uint8_t  g_cal_lo;
static uint8_t  g_cal_valid;

/* Число чтений флагов, в которых датчик не смог выбрать направление (одновременно
 * установлены обе части антагонистической пары, например up и down). Диагностика
 * качества распознавания: paj_getAmbiguousCount(). */
static uint8_t g_ambiguous;

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
    g_ticks_per_ms = (uint16_t)(SystemCoreClock / 8UL / 1000UL); /* HCLK/8 */
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

/* Начальная конфигурация (datasheet "i. Initial") — 47 записей. */
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
/* Режим proximity (datasheet "iii. Change to PS mode") — 26 записей. */
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

/* Сброс состояния жеста (кэш + счётчики). */
static void gesture_reset(void) {
    g_gesture     = GESTURE_NONE;
    g_has_gesture = 0;
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

/* Запись порогов gesture engine по осям (REG_GEST_TH_X/Y, банк 0).
 * Вынесено из таблицы инициализации, потому что в официальной
 * последовательности datasheet этих регистров нет: там остаются заводские
 * значения, и в PAJ7620U2 (как и в любом другом экземпляре) они могут
 * отличаться. Явная запись делает чувствительность к LEFT/RIGHT и UP/DOWN
 * настраиваемой (PAJ_XDIR_THRESHOLD / PAJ_YDIR_THRESHOLD). */
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
 * пороги gesture engine и proximity, включение и маски прерываний.
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

/* Переинициализация датчика после серии сбоев I2C: полный цикл wake-up и
 * конфигурации, включая восстановление порогов. */
static bool sensor_reinit(void) {
    Delay_Ms(1);                 /* T1 > 700 мкс */
    wakeup_cmd();
    if (!wait_ready()) {
        return false;
    }
    return configure_sensor();
}

/* Восстановление после сбоя I2C.
 *
 * Одиночный сбой не переинициализирует датчик: PAJ_REINIT_THRESHOLD сбоев подряд
 * отделяют «шина качнулась» от «датчик действительно завис». Это важно для
 * устойчивости жестов — перезапись всей конфигурации занимает единицы
 * миллисекунд, в течение которых датчик не отвечает и результаты теряются.
 *
 * Счётчик переинициализаций сохраняется между сбоями и сбрасывается после
 * любого успешного чтения датчика. Провал sensor_reinit() немедленно
 * отражается как PAJ_ERR_SENSOR_HANG и не маскируется под PAJ_ERR_I2C: иначе
 * по публичному API невозможно отличить «сбой чтения» от «сбоя чтения +
 * неудачное восстановление». */
static void recover_sensor(void) {
    g_fail_streak++;
    if (g_fail_streak < PAJ_REINIT_THRESHOLD) {
        return;
    }
    g_fail_streak = 0;

    gesture_reset();

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
 * называет UP, физически является RIGHT (и так далее по кругу). */
#if PAJ_GESTURE_ROTATE == 1
#  define PAJ_DIR_UP        GESTURE_RIGHT
#  define PAJ_DIR_DOWN      GESTURE_LEFT
#  define PAJ_DIR_LEFT      GESTURE_UP
#  define PAJ_DIR_RIGHT     GESTURE_DOWN
#  define PAJ_DIR_CW        GESTURE_CCW
#  define PAJ_DIR_CCW       GESTURE_CW
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
#  define PAJ_DIR_CW        GESTURE_CCW
#  define PAJ_DIR_CCW       GESTURE_CW
#else
#  define PAJ_DIR_UP        GESTURE_UP
#  define PAJ_DIR_DOWN      GESTURE_DOWN
#  define PAJ_DIR_LEFT      GESTURE_LEFT
#  define PAJ_DIR_RIGHT     GESTURE_RIGHT
#  define PAJ_DIR_CW        GESTURE_CW
#  define PAJ_DIR_CCW       GESTURE_CCW
#endif

/* Декодирование флагов датчика в тип жеста.
 * Датчик выставляет битовые флаги (0x43/0x44), а не код; при нескольких
 * установленных битах выбирается младший — так поведение детерминировано
 * и не зависит от порядка регистровых чтений.
 * forward/backward/wave не зависят от ориентации модуля. */
static gesture_t decode_gesture(uint8_t flag1, uint8_t flag2) {
    gesture_t g;

    if      (flag1 & GES_UP)        g = PAJ_DIR_UP;
    else if (flag1 & GES_DOWN)      g = PAJ_DIR_DOWN;
    else if (flag1 & GES_LEFT)      g = PAJ_DIR_LEFT;
    else if (flag1 & GES_RIGHT)     g = PAJ_DIR_RIGHT;
    else if (flag1 & GES_FORWARD)   g = GESTURE_FORWARD;
    else if (flag1 & GES_BACKWARD)  g = GESTURE_BACKWARD;
    else if (flag1 & GES_CW)        g = PAJ_DIR_CW;
    else if (flag1 & GES_CCW)       g = PAJ_DIR_CCW;
    else if (flag2 & INT2_WAVE_FLAG) g = GESTURE_WAVE;
    else                            g = GESTURE_NONE;

    return g;
}

/* true, если флаги содержат обе части хотя бы одной антагонистической пары.
 * Одно физическое движение не может быть одновременно вверх и вниз, поэтому
 * такой набор битов означает неуверенную классификацию датчика, а не жест.
 * Считается всегда, влияние на результат задаёт PAJ_REJECT_AMBIGUOUS. */
static bool flags_contradictory(uint8_t f1) {
    return ((f1 & GES_PAIR_UP_DOWN)    == GES_PAIR_UP_DOWN)    ||
           ((f1 & GES_PAIR_LEFT_RIGHT) == GES_PAIR_LEFT_RIGHT) ||
           ((f1 & GES_PAIR_FWD_BACK)   == GES_PAIR_FWD_BACK)   ||
           ((f1 & GES_PAIR_ROTATE)     == GES_PAIR_ROTATE);
}

/* Сохранить декодированный жест в кэш: датчик сбрасывает флаги при чтении,
 * поэтому пережить чтение может только кэш. */
static void cache_gesture(gesture_t g) {
    if (g == GESTURE_NONE) {
        return;
    }
    g_gesture     = g;
    g_has_gesture = 1;
}

/* Чтение флагов жеста (REG_INT_FLAG1 + REG_INT_FLAG2) с декодированием.
 * ВНИМАНИЕ: чтение этих регистров сбрасывает флаги в датчике, поэтому
 * найденный жест кладётся в кэш — читатель заберёт его paj_readGesture().
 *
 * REG_INT_FLAG1 читается и декодируется ПЕРВЫМ и отдельно от 0x44: если
 * после этого чтение 0x44 сорвётся, жест из 0x43 уже нельзя ни перечитать,
 * ни восстановить, и он был бы молча потерян вместе с кэшем. Такой сбой
 * стоит только флага wave (он лежит в 0x44), а не всего результата.
 *
 * Возвращает: true если оба регистра прочитаны. */
static bool poll_gesture(void) {
    uint8_t   f1  = 0;
    uint8_t   f2  = 0;
    gesture_t g   = GESTURE_NONE;
    bool      bad = false;
    bool      f1_ok;

    f1_ok = rd(PAJ_B0, REG_INT_FLAG1, &f1);
    if (f1_ok) {
        bad = flags_contradictory(f1);
        if (bad) {
            g_ambiguous++;
        }
        if (!bad || !PAJ_REJECT_AMBIGUOUS) {
            /* Флаг wave живёт в 0x44, поэтому здесь он не учитывается:
             * приоритет «направление важнее wave» держится за счёт порядка. */
            g = decode_gesture(f1, 0x00);
            cache_gesture(g);
        }
    }

    if (!rd(PAJ_B0, REG_INT_FLAG2, &f2)) {
        return false;
    }
    if (!f1_ok) {
        return false;
    }
    if (f1 == 0x00) {
        /* Направлений нет — единственный источник жеста здесь wave. */
        g = decode_gesture(0x00, f2);
        cache_gesture(g);
    }

    /* Оба регистра прочитаны — шина жива, цепочка сбоев прерывается. */
    g_fail_streak = 0;

#ifdef PAJ7620U2_DEBUG
    if (g != GESTURE_NONE || bad) {
        printf("GES: %02X/%02X%s -> %d\r\n", f1, f2, bad ? " AMB" : "", (int)g);
    }
#endif
    return true;
}

/* ============================================================================
 * КАЛИБРОВКА ПОРОГОВ PROXIMITY
 * ============================================================================ */

#if PAJ_ENABLE_CALIBRATION

/* Целочисленный квадратный корень (алгоритм Ньютона).
 * Используется при калибровке для вычисления σ.
 * Возвращает: floor(√n). */
static uint8_t isqrt(uint16_t n) {
    if (n == 0) {
        return 0;
    }
    uint16_t x = n;
    uint16_t y = (uint16_t)((x + 1) / 2);
    while (y < x) {
        x = y;
        y = (uint16_t)((x + n / x) / 2);
    }
    return (uint8_t)x;
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

    if (valid < PAJ_CAL_SAMPLES / 2) {
        g_cal_valid = 0;
#ifdef PAJ7620U2_DEBUG
        printf("CAL: no PS data (valid=%d), keep cfg thresholds %d/%d\r\n",
               valid, PAJ_PROX_THRESHOLD, PAJ_PROX_EXIT_TH);
#endif
        return true;
    }

    /* Медиана (чётное число замеров — среднее двух центральных). */
    sort_u8(buf, valid);
    uint8_t median;
    if (valid & 1) {
        median = buf[valid / 2];
    } else {
        median = (uint8_t)(((uint16_t)buf[valid / 2 - 1] + (uint16_t)buf[valid / 2]) / 2);
    }

    /* Дисперсия от медианы (с отклонением /4, чтобы избежать переполнения). */
    uint32_t sum_sq = 0;
    for (uint8_t i = 0; i < valid; i++) {
        int16_t d  = (int16_t)buf[i] - (int16_t)median;
        int16_t d4 = d / 4;
        sum_sq += (uint16_t)(d4 * d4);
    }
    uint8_t sigma = (uint8_t)(isqrt((uint16_t)(sum_sq / valid)) * 4);

    uint16_t entry  = (uint16_t)median + (uint16_t)PAJ_CAL_SIGMA_COEFF * sigma;
    uint16_t exit_th = entry * 3 / 4;   /* гистерезис: выход ниже входа */

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
            /* Датчик сейчас в режиме жестов, где REG_PS_DATA не отражает
             * сигнал proximity, поэтому пороги остаются заданными в
             * конфигурации. Калибровка выполняется явно через
             * paj_setProximityMode() + paj_recalibrate(). */
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

bool paj_available(void) {
    if (!poll_gesture()) {
        g_last_error = PAJ_ERR_I2C;
        recover_sensor();
        /* Жест из успешно прочитанного REG_INT_FLAG1 не теряется из-за
         * сбоя чтения REG_INT_FLAG2: он уже в кэше, а recover_sensor()
         * сбрасывает кэш только при реальной переинициализации. */
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
            /* Жест из успешно прочитанного REG_INT_FLAG1 достовернее ошибки
             * чтения REG_INT_FLAG2: отдаём его, а ошибка остаётся видимой
             * через paj_getLastError(). */
            if (g_has_gesture) {
                g_has_gesture = 0;
                return g_gesture;
            }
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
