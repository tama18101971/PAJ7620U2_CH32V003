/*
 * paj_int_config.c — Реализация EXTI для PAJ7620U2 (INT → PC3)
 */

#include "paj_int_config.h"
#include <ch32v00x.h>

/* Флаг прерывания: ISR ставит =1, main loop сбрасывает =0 */
volatile uint8_t g_paj_int_flag = 0;

void paj_exti_init(void) {
    /* Включаем AFIO clock (обязательно для EXTI маппинга) */
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_AFIO, ENABLE);

    /* PC3 — вход с подтяжкой к VCC (INT open-drain active-low) */
    GPIO_InitTypeDef gpio = {0};
    gpio.GPIO_Pin   = PAJ_INT_PIN;
    gpio.GPIO_Mode  = GPIO_Mode_IPU;
    GPIO_Init(PAJ_INT_PORT, &gpio);

    /* Маппинг PC3 → EXTI_Line3 */
    GPIO_EXTILineConfig(PAJ_INT_PORT_SOURCE, PAJ_INT_PIN_SOURCE);

    /* EXTI: falling-edge trigger (INT low при распознанном жесте) */
    EXTI_InitTypeDef exti = {0};
    exti.EXTI_Line    = PAJ_INT_LINE;
    exti.EXTI_Mode    = EXTI_Mode_Interrupt;
    exti.EXTI_Trigger = EXTI_Trigger_Falling;
    exti.EXTI_LineCmd = ENABLE;
    EXTI_Init(&exti);

    /* NVIC: EXTI7_0_IRQn (общий для EXTI 0-7) */
    NVIC_InitTypeDef nvic = {0};
    nvic.NVIC_IRQChannel                   = EXTI7_0_IRQn;
    nvic.NVIC_IRQChannelPreemptionPriority = 1;
    nvic.NVIC_IRQChannelSubPriority       = 0;
    nvic.NVIC_IRQChannelCmd               = ENABLE;
    NVIC_Init(&nvic);
}

void paj_exti_enable(void) {
    NVIC_EnableIRQ(EXTI7_0_IRQn);
}

void paj_exti_disable(void) {
    NVIC_DisableIRQ(EXTI7_0_IRQn);
}

/* ============================================================================
 * Обработка прерывания PAJ7620U2 — проверяет EXTI_Line3 и ставит флаг.
 * НЕ сбрасывает pending bit — вызывайте paj_clear_exti() после.
 * ============================================================================ */
void paj_handle_exti(void) {
    if (EXTI_GetITStatus(PAJ_INT_LINE) != RESET) {
        g_paj_int_flag = 1;
    }
}

/* ============================================================================
 * Сброс pending bit EXTI_Line3
 * ============================================================================ */
void paj_clear_exti(void) {
    EXTI_ClearITPendingBit(PAJ_INT_LINE);
}

/* ============================================================================
 * User callback — слабый, пользователь может переопределить
 * ============================================================================ */
void paj_exti_callback(void) __attribute__((weak));
void paj_exti_callback(void) {}

/* ============================================================================
 * ISR: EXTI7_0_IRQHandler
 *
 * Все EXTI 0-7 делят один вектор на CH32V003. Обработчик намеренно strong:
 * NoneOS-SDK также предоставляет weak fallback, который зациклен. Если и этот
 * обработчик объявить weak, линкер может выбрать fallback SDK, и первое EXTI
 * приведёт к вечному зависанию МК.
 * ============================================================================ */
#if PAJ_PROVIDE_EXTI_ISR
void EXTI7_0_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));

void EXTI7_0_IRQHandler(void) {
    paj_handle_exti();
    paj_clear_exti();
    paj_exti_callback();
}
#endif
