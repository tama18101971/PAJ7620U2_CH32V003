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

/* Опрашивание датчика и вывод результата.
 * Цикл ограничен: датчик хранит только последний жест, зависшая шина не
 * должна превращать обработчик в вечный цикл. */
static void drain_sensor(void) {
    uint8_t guard = 8;
    while (guard-- && paj_available()) {
        gesture_t g = paj_readGesture();
        if (g != GESTURE_NONE) {
            print_gesture(g);
        }
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

        if (paj_available()) {
            gesture_t g = paj_readGesture();
            print_gesture(g);
            drain_sensor();
        } else if (paj_getLastError() == PAJ_ERR_SENSOR_HANG) {
            handle_sensor_failure();
        }
    }

#else
    /* ========================================================================
     * POLLING MODE
     * ======================================================================== */
    while (1) {
        if (paj_available()) {
            gesture_t g = paj_readGesture();
            print_gesture(g);
            drain_sensor();
        } else if (paj_getLastError() == PAJ_ERR_SENSOR_HANG) {
            handle_sensor_failure();
        }
    }
#endif
}

void NMI_Handler(void) {}
void HardFault_Handler(void) { while (1) {} }
