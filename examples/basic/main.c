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

/* 1 = печатать состояние автомата датчика и счётчики на каждый жест, а после
 * инициализации вывести фактические пороги gesture engine. */
#ifndef PAJ_EXAMPLE_TRACE
#define PAJ_EXAMPLE_TRACE      0
#endif

/* Обработчики прерываний (обязательны для CH32V003) */
void NMI_Handler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
void HardFault_Handler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
void TIM2_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));

/* Независимая от Delay_Ms() временная база: TIM2 каждую 1 мс увеличивает
 * программный счётчик. SysTick остаётся полностью в распоряжении SDK. */
static volatile uint32_t g_millis;

static void timebase_init(void) {
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM2, ENABLE);

    TIM_TimeBaseInitTypeDef tim;
    TIM_TimeBaseStructInit(&tim);
    tim.TIM_Prescaler = (uint16_t)(SystemCoreClock / 1000000UL - 1UL);
    tim.TIM_Period    = 1000 - 1;
    TIM_TimeBaseInit(TIM2, &tim);
    TIM_ClearITPendingBit(TIM2, TIM_IT_Update);
    TIM_ITConfig(TIM2, TIM_IT_Update, ENABLE);

    NVIC_InitTypeDef nvic = {0};
    nvic.NVIC_IRQChannel                   = TIM2_IRQn;
    nvic.NVIC_IRQChannelPreemptionPriority = 1;
    nvic.NVIC_IRQChannelSubPriority        = 0;
    nvic.NVIC_IRQChannelCmd                = ENABLE;
    NVIC_Init(&nvic);
    TIM_Cmd(TIM2, ENABLE);
}

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
static uint16_t s_gestures;

static uint8_t sensor_state(void) {
    uint8_t v = 0xFF;
    if (!paj_readStatus(&v)) {
        return 0xFF;
    }
    return (uint8_t)(v & 0x03);
}

static void trace_line(const char *tag) {
    printf("%s ges=%u amb=%u st=%u err=%d i2c=%d reinit=%u\r\n",
           tag, (unsigned)s_gestures,
           (unsigned)paj_getAmbiguousCount(), (unsigned)sensor_state(),
           paj_getLastError(), paj_getLastI2CStatus(),
           (unsigned)paj_getReinitCount());
}

/* Фактические пороги gesture engine; bank 0 остаётся выбранным. */
static void dump_gesture_regs(void) {
    static const uint8_t regs[] = {
        0x83, 0x8B, 0x90, 0x93, 0x94, 0x95,
        0x96, 0x97, 0x9A, 0x9C, 0x9F, 0xCF, 0xD0,
    };
    if (i2c_write_register(PAJ7620_I2C_ADDR, REG_BANK_SEL, PAJ_B0) != I2C_OK) {
        printf("regs: bank0 select failed\r\n");
        return;
    }
    printf("regs:");
    for (uint8_t i = 0; i < (uint8_t)sizeof(regs); i++) {
        uint8_t v = 0xFF;
        if (i2c_read_register(PAJ7620_I2C_ADDR, regs[i], &v) != I2C_OK) {
            v = 0xFF;
        }
        printf(" %02X=%02X", regs[i], v);
    }
    printf("\r\n");
}
#endif

/* Сброс стартовых флагов без печати и без блокирующего ожидания жеста. */
static void drain_sensor(void) {
    uint8_t guard = 8;
    while (guard-- && paj_available()) {
        (void)paj_takeGesture();
    }
}

/* Диагностика отказа датчика и попытка восстановления. */
static void handle_sensor_failure(void) {
    printf("SENSOR HANG (i2c=%d, failures=%d), re-init...\r\n",
           paj_getLastI2CStatus(), paj_getReinitCount());
    if (paj_init()) {
        printf("Sensor recovered.\r\n");
    } else {
        printf("Re-init failed (err=%d)\r\n", paj_getLastError());
    }
}

/* Один неблокирующий шаг обработки. Восстановление после неисправности
 * выполняется явно и не скрыто внутри регулярного опроса. */
static void service_sensor(void) {
    if (paj_poll(g_millis)) {
        gesture_t g = paj_takeGesture();
        print_gesture(g);
#if PAJ_EXAMPLE_TRACE
        s_gestures++;
        trace_line("HIT");
#endif
    } else if (paj_getLastError() == PAJ_ERR_SENSOR_HANG) {
        handle_sensor_failure();
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
    timebase_init();
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

    /* TIM2 будит МК каждую 1 мс, EXTI будит дополнительно по событию INT.
     * paj_poll() сам ограничивает I2C-опрос интервалом 5 мс; подтверждение
     * бокового жеста завершается по времени даже без нового фронта INT. */
    while (1) {
        __WFI();
#if PAJ_INT_MODE == 1
        g_paj_int_flag = 0;
#endif
        service_sensor();
    }
}

void TIM2_IRQHandler(void) {
    if (TIM_GetITStatus(TIM2, TIM_IT_Update) != RESET) {
        TIM_ClearITPendingBit(TIM2, TIM_IT_Update);
        g_millis++;
    }
}

void NMI_Handler(void) {}
void HardFault_Handler(void) { while (1) {} }
