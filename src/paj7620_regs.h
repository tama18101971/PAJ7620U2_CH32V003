#ifndef PAJ7620_REGS_H
#define PAJ7620_REGS_H

/*
 * paj7620_regs.h — карта регистров PAJ7620U2.
 *
 * Источник: docs/PAJ7620U2_DS.pdf (PixArt, DOCUMENT CONTROLLED V0.8 2014/06/11),
 * раздел "Register Map and Function" / "Register Function Description".
 * В отличие от PAJ7620 (не-U2) у PAJ7620U2 нет регистра PART_ID: идентификация
 * выполняется по регистру состояния пробуждения 0x00 (см. REG_WAKEUP_STAT).
 *
 * Адресное пространство разделено на два банка, переключаются регистром
 * REG_BANK_SEL (0xEF) в обоих банках. Адреса 0x00..0x6E в банке 0 и
 * 0x01..0x7F в банке 1 НЕ пересекаются по смыслу, поэтому каждая транзакция
 * требует явно указанного банка (см. rd()/wr() в paj7620.c).
 */

#include <stdint.h>

/* 7-битный адрес ведомого (datasheet: "I2C Slave ID ... 0x73"). */
#define PAJ7620_I2C_ADDR       0x73

/* Номера банков регистров. */
#define PAJ_B0                 0
#define PAJ_B1                 1

/* ============================================================================
 * Регистры банка 0
 * ============================================================================ */

#define REG_BANK_SEL           0xEF  /* выбор банка: 0 = bank 0, 1 = bank 1 */
#define REG_WAKEUP_STAT        0x00  /* состояние пробуждения: PAJ_WAKEUP_DONE */
#define REG_SUSPEND            0x03  /* команда suspend: записать 0x01 */
#define REG_INT1_EN            0x41  /* маска прерываний по жестам (8 бит) */
#define REG_INT2_EN            0x42  /* бит1 = маска PS, бит0 = маска wave */
#define REG_INT_FLAG1          0x43  /* флаги жестов, сбрасываются чтением */
#define REG_INT_FLAG2          0x44  /* бит1 = флаг PS, бит0 = флаг wave */
#define REG_STATE              0x45  /* биты[1:0] — состояние автомата датчика */
#define REG_PS_TH_HIGH         0x69  /* верхний порог гистерезиса PS */
#define REG_PS_TH_LOW          0x6A  /* нижний порог гистерезиса PS */
#define REG_PS_APPROACH        0x6B  /* бит0 = объект в зоне срабатывания */
#define REG_PS_DATA            0x6C  /* 8-битные данные PS (0..255) */
#define REG_OBJ_BRIGHTNESS     0xB0  /* яркость объекта (0..255) */
#define REG_OBJ_SIZE_L         0xB1  /* размер объекта, младший байт */
#define REG_OBJ_SIZE_H         0xB2  /* размер объекта, старший байт (0..900) */

/* ============================================================================
 * Регистры банка 1
 * ============================================================================ */

#define REG_ENABLE             0x72  /* бит0 = 1: включить PAJ7620U2 */
#define REG_PS_GAIN            0x44  /* биты[5:0] — усиление PS (0 = 1x, 1 = 2x) */
#define REG_STEP_S1_L          0x67  /* IDLE S1 Step [7:0]  (S1, response factor) */
#define REG_STEP_S1_H          0x68  /* IDLE S1 Step [15:8] */
#define REG_STEP_S2_L          0x69  /* IDLE S2 Step [7:0]  (S2, response factor) */
#define REG_STEP_S2_H          0x6A  /* IDLE S2 Step [15:8] */
#define REG_STEP_OPS1_L        0x6B  /* OPtoS1 Step [7:0]   (время OP -> S1) */
#define REG_STEP_OPS1_H        0x6C  /* OPtoS1 Step [15:8] */
#define REG_STEP_S1S2_L        0x6D  /* S1toS2 Step [7:0]   (время S1 -> S2) */
#define REG_STEP_S1S2_H        0x6E  /* S1toS2 Step [15:8] */

/* ============================================================================
 * Битовые определения
 * ============================================================================ */

/* REG_INT_FLAG1 / REG_INT1_EN (0x43 / 0x41): битовые флаги жестов.
 * Порядок битов НЕ задаёт временной порядок событий. При составном наборе
 * драйвер явно приоритизирует Forward/Backward над боковыми жестами
 * (см. decode_gesture() в paj7620.c). */
#define GES_UP                 0x01  /* [0] жест вверх */
#define GES_DOWN               0x02  /* [1] жест вниз */
#define GES_LEFT               0x04  /* [2] жест влево */
#define GES_RIGHT              0x08  /* [3] жест вправо */
#define GES_FORWARD            0x10  /* [4] приближение (push) */
#define GES_BACKWARD           0x20  /* [5] отведение (pull) */
#define GES_CW                 0x40  /* [6] круг по часовой стрелке */
#define GES_CCW                0x80  /* [7] круг против часовой стрелки */

/* REG_INT2_EN (0x42) — маски: [1] = PS, [0] = wave */
#define INT2_WAVE_MASK         0x01
#define INT2_PS_MASK           0x02

/* REG_INT_FLAG2 (0x44) — флаги: [1] = PS, [0] = wave (та же раскладка,
 * что и у масок, но это флаги событий, а не разрешения) */
#define INT2_WAVE_FLAG         0x01
#define INT2_PS_FLAG           0x02

/* REG_STATE (0x45), биты [1:0] */
#define STATE_MASK             0x03
#define STATE_OP               0x00  /* Operation state: 120/240 Гц, жесты */
#define STATE_S1               0x01  /* Standby 1: замедленное детектирование */
#define STATE_S2               0x02  /* Standby 2: минимальное детектирование */

/* REG_ENABLE (bank 1, 0x72) */
#define EN_ENABLE              0x01  /* бит0 = 1: датчик включён */

/* REG_WAKEUP_STAT (0x00): datasheet — "It will return 0x20 when wake-up
 * finish. By the way, There is no-ack from PAJ7620U2 before wake-up finish."
 * Чтение 0xFF при успешном ACK означает, что по этому адресу отвечает не
 * PAJ7620U2 (другое устройство / зависшая шина) — драйвер в этом случае
 * возвращает PAJ_ERR_INVALID_ID. */
#define PAJ_WAKEUP_DONE        0x20

#endif /* PAJ7620_REGS_H */
