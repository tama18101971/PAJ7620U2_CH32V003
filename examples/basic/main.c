/*
 * main.c — Пример использования драйвера PAJ7620U2
 *
 * Поддерживает два режима работы:
 *   PAJ_INT_MODE=0 — Polling (опрос через paj_available())
 *   PAJ_INT_MODE=1 — Interrupt (INT → PC3, EXTI3 falling-edge)
 *
 * Подключение (совпадает с драйвером APDS-9960):
 *   PC1 = SDA, PC2 = SCL (I2C)
 *   PC3 = INT (PAJ7620U2, input pull-up)
 *   PD5 = TX (UART 115200)
 */

#include <ch32v00x.h>
#include <debug.h>
#include "i2c.h"
#include "paj7620.h"
#include "paj7620_regs.h"

#if PAJ_INT_MODE == 1
#include "paj_int_config.h"
#endif

/* Демонстрация proximity: 1 = после инициализации датчик кратковременно
 * переводится в режим proximity, читаются PS-данные и калибруются пороги,
 * затем датчик возвращается в режим жестов.
 * Нужно для проверки paj_setProximityMode()/paj_readProximity()/
 * paj_recalibrate(): в режиме жестов датчик PS-данные не обновляет. */
#ifndef PAJ_EXAMPLE_PS_MODE
#define PAJ_EXAMPLE_PS_MODE    0
#endif

/* Диагностический вывод: 1 = на каждое событие печатается состояние
 * автомата датчика (REG_STATE 0x45) и накопленные счётчики, что позволяет
 * отличить «датчик не распознал жест» (событие без жеста) от «распознал, но
 * не тот» (несколько битов флагов одновременно). Для сырых байтов флагов
 * 0x43/0x44 дополнительно включите PAJ7620U2_DEBUG.
 * В серийной прошивке оставляйте 0. */
#ifndef PAJ_EXAMPLE_TRACE
#define PAJ_EXAMPLE_TRACE     0
#endif

/* Обработчики прерываний (обязательны для CH32V003) */
void NMI_Handler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
void HardFault_Handler(void) __attribute__((interrupt("WCH-Interrupt-fast")));

/* Вывод имени распознанного жеста в UART (общий код для polling/interrupt) */
static void print_gesture(gesture_t g) {
    switch (g) {
        case GESTURE_LEFT:     printf("LEFT\r\n");     break;
        case GESTURE_RIGHT:    printf("RIGHT\r\n");    break;
        case GESTURE_UP:       printf("UP\r\n");       break;
        case GESTURE_DOWN:     printf("DOWN\r\n");     break;
        case GESTURE_FORWARD:  printf("FORWARD\r\n"); break;
        case GESTURE_BACKWARD: printf("BACKWARD\r\n"); break;
        case GESTURE_CW:       printf("CW\r\n");       break;
        case GESTURE_CCW:      printf("CCW\r\n");      break;
        case GESTURE_WAVE:     printf("WAVE\r\n");     break;
        default: break;
    }
}

#if PAJ_EXAMPLE_TRACE
/* Счётчики диагностики (только при PAJ_EXAMPLE_TRACE=1). */
static uint16_t s_events;      /* событий: прерываний (INT) либо пустых опросов */
static uint16_t s_gestures;    /* декодированных жестов */

/* Состояние автомата датчика: 0 = OP (жесты, 120/240 Гц), 1 = S1, 2 = S2. */
static uint8_t sensor_state(void) {
    uint8_t v = 0xFF;
    if (!paj_readStatus(&v)) {
        return 0xFF;
    }
    return (uint8_t)(v & 0x03u);
}

static void trace_line(const char *tag) {
    printf("%s ev=%u ges=%u amb=%u st=%u err=%d i2c=%d reinit=%u\r\n",
           tag, (unsigned)s_events, (unsigned)s_gestures,
           (unsigned)paj_getAmbiguousCount(),
           (unsigned)sensor_state(), paj_getLastError(),
           paj_getLastI2CStatus(), paj_getReinitCount());
}

/* Дамп регистров gesture engine после paj_init() — показывает фактические
 * пороги чувствительности, действующие на датчике. Банк 0 выбирается явно и
 * остаётся выбранным, поэтому кэш банка в драйвере остаётся корректным.
 *
 * Соответствие имён (datasheet V0.8, "Register Map", банк 0):
 *   0x83 R_LightThd[7:0]        0x93 R_XDirectionThd[4:0]
 *   0x8B R_Cursor_ObjectSizeThd  0x94 R_YDirectionThd[4:0]
 *   0x90 R_NoMotionCountThd[6:0] 0x95 R_ZDirectionThd[4:0]
 *   0x96 R_ZDirectionXYThd[4:0]  0x97 R_ZDirectionAngleThd[3:0]
 *   0x9A R_RotateXYThd[4:0]      0x9C R_FilterDistThd[6:2]/R_FilterWeight[1:0]
 *   0x9F R_UseBGModel/RotateEnh/ZDirectionEnh/YDirectionEnh/XDirectionEnh
 *   0xCF R_WaveAngleThd[7:4]/R_WaveCountThd[3:0]
 *   0xD0 R_AbortXYRatio[7:3]/R_AbortCountThd[2:0]
 */
static void dump_gesture_regs(void) {
    static const uint8_t regs[] = {
        0x83, 0x8B, 0x90, 0x93, 0x94, 0x95, 0x96, 0x97,
        0x9A, 0x9C, 0x9F, 0xCF, 0xD0,
    };

    if (i2c_write_register(PAJ7620_I2C_ADDR, REG_BANK_SEL, PAJ_B0) != I2C_OK) {
        printf("regs: bank0 select failed\r\n");
        return;
    }

    printf("regs:");
    for (uint8_t i = 0; i < (uint8_t)(sizeof(regs) / sizeof(regs[0])); i++) {
        uint8_t v = 0xFF;
        if (i2c_read_register(PAJ7620_I2C_ADDR, regs[i], &v) != I2C_OK) {
            v = 0xFF;
        }
        printf(" %02X=%02X", regs[i], v);
    }
    printf("\r\n");
}
#endif

/* Вывод распознанного жеста и учёт для диагностики. */
static void report_gesture(gesture_t g) {
    print_gesture(g);
#if PAJ_EXAMPLE_TRACE
    if (g != GESTURE_NONE) {
        s_gestures++;
    }
#endif
}

/* Опрашивание датчика и вывод результата.
 * Цикл ограничен: датчик хранит только последний жест, зависшая шина не
 * должна превращать обработчик в вечный цикл. */
static void drain_sensor(void) {
    uint8_t guard = 8;
    while (guard-- && paj_available()) {
        report_gesture(paj_readGesture());
    }
}

/* Диагностика отказа датчика и попытка восстановления. */
static void handle_sensor_failure(void) {
    printf("SENSOR HANG (i2c=%d, reinit=%d), re-init...\r\n",
           paj_getLastI2CStatus(), paj_getReinitCount());
    if (paj_init()) {
        printf("Sensor recovered.\r\n");
    } else {
        printf("Re-init failed (err=%d)\r\n", paj_getLastError());
    }
}

/* Обработка одного события датчика — общий путь для обоих режимов.
 * В interrupt-режиме вызывается после пробуждения по INT, в polling —
 * каждый проход главного цикла. */
static void handle_event(void) {
#if PAJ_EXAMPLE_TRACE
    s_events++;
#endif
    if (paj_available()) {
        report_gesture(paj_readGesture());
#if PAJ_EXAMPLE_TRACE
        trace_line("HIT");
#endif
        drain_sensor();
        return;
    }

    if (paj_getLastError() == PAJ_ERR_SENSOR_HANG) {
        handle_sensor_failure();
        return;
    }

#if PAJ_EXAMPLE_TRACE
    /* Прерывание пришло, а жеста нет: движение не распознано датчиком либо
     * результат уже забран предыдущим чтением флагов. */
    trace_line("MISS");
#endif
}

/* Демонстрация режима proximity: чтение PS-данных и калибровка порогов. */
#if PAJ_EXAMPLE_PS_MODE
static void demo_proximity(void) {
    if (!paj_setProximityMode()) {
        printf("PS mode switch failed (err=%d)\r\n", paj_getLastError());
        (void)paj_setGestureMode();
        return;
    }

    uint8_t v;
    if (paj_readStatus(&v)) {
        printf("PS mode: sensor state=%d\r\n", v & 0x03);
    }
    if (!paj_recalibrate()) {
        printf("PS recalibration failed (err=%d)\r\n", paj_getLastError());
    }
    for (uint8_t i = 0; i < 5; i++) {
        if (paj_readProximity(&v)) {
            printf("PS data=%d\r\n", v);
        }
        Delay_Ms(100);      /* датчик обновляет PS-данные с частотой 10 Гц */
    }

    if (!paj_setGestureMode()) {
        printf("Gesture mode switch failed (err=%d)\r\n", paj_getLastError());
    }
}
#endif /* PAJ_EXAMPLE_PS_MODE */

int main(void) {
    SystemCoreClockUpdate();
    Delay_Init();
    USART_Printf_Init(115200);
    printf("PAJ7620U2 gesture driver init...\r\n");

    uint8_t i2c_st = i2c_init(400000);
    if (i2c_st != I2C_OK) {
        printf("ERROR: i2c_init failed (%d)\r\n", i2c_st);
        while (1) {}
    }

    if (!paj_init()) {
        printf("ERROR: PAJ7620U2 not responding! err=%d i2c=%d\r\n",
               paj_getLastError(), paj_getLastI2CStatus());

        /* Автосканирование шины для диагностики: сенсор на 0x73? */
        printf("Scanning I2C bus...\r\n");
        uint8_t found = 0;
        for (uint8_t a = 1; a < 0x7F; a++) {
            if (i2c_probe_address(a, NULL, NULL) == I2C_OK) {
                printf("  device found at 0x%02X\r\n", a);
                found++;
            }
        }
        if (found == 0) {
            printf("  no devices on the bus — check wiring/power/pull-ups\r\n");
        }

        while (1) {}
    }

#if PAJ_INT_MODE == 1
    printf("Mode: INTERRUPT (INT=PC3)\r\n");
    paj_exti_init();
    paj_enableInterrupt();
#else
    printf("Mode: POLLING\r\n");
#endif

    printf("PAJ7620U2 ready.\r\n");

    Delay_Ms(200);

#if PAJ_EXAMPLE_TRACE
    dump_gesture_regs();
#endif

    /* Сброс возможных жестов, распознанных во время инициализации */
    drain_sensor();

#if PAJ_EXAMPLE_PS_MODE
    demo_proximity();
    drain_sensor();
#endif

#if PAJ_INT_MODE == 1
    /* ========================================================================
     * INTERRUPT MODE
     *
     * Main loop засыпает (__WFI) пока ISR не установит g_paj_int_flag.
     * Прерывание срабатывает при распознавании жеста, разрешённого масками
     * (falling edge на INT pin). После пробуждения читаем жест штатным API.
     * ======================================================================== */
    while (1) {
        while (g_paj_int_flag == 0) {
            __WFI();
        }
        g_paj_int_flag = 0;
        handle_event();
    }

#else
    /* ========================================================================
     * POLLING MODE
     * ======================================================================== */
    while (1) {
#if PAJ_EXAMPLE_TRACE
        /* Пауза между опросами: в режиме диагностики шина не должна
         * заниматься сплошным чтением, иначе страдает сам замер. */
        Delay_Ms(1);
#endif
        handle_event();
    }
#endif
}

void NMI_Handler(void) {}
void HardFault_Handler(void) { while (1) {} }
