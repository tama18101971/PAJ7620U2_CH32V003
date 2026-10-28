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

/* Пороги gesture engine по осям (datasheet V0.8, "Register Map").
 * В официальную начальную последовательность ("i. Initial") эти регистры НЕ
 * входят, поэтому в датчике остаются заводскими и настройке через таблицу
 * инициализации не поддаются; драйвер записывает их отдельным шагом
 * (apply_axis_thresholds() в paj7620.c). */
#define REG_GEST_TH_X          0x93  /* биты[4:0] — порог влево/вправо */
#define REG_GEST_TH_Y          0x94  /* биты[4:0] — порог вверх/вниз */
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

/* Тайминги автомата состояний (Operation → S1 → S2). Имена и назначение — по
 * V0.8 (docs/PAJ7620U2_DS.pdf, стр. 40); в V0.7 те же адреса названы
 * «IDLE S1/S2 Step, OPtoS1/S1toS2 Step» (response factor, см.
 * docs/PAJ7620U2_datasheet.pdf, стр. 7/11) — старые имена макросов сохранены.
 *
 * R_IDLE_TIME задаёт простой до попытки ухода в сон в рабочем состоянии;
 * R_Obj_TIME_1/2 — время (в кадрах отчёта) до входа в слабый/глубокий сон.
 * После пробуждения из S1/S2 датчик замедляется, и быстрые жесты теряются:
 * если жест «не срабатывает» первым после паузы, увеличивайте R_Obj_TIME_*
 * (значения в g_init_seq/g_gesture_seq в paj7620.c). */
#define REG_IDLE_TIME_L        0x65  /* R_IDLE_TIME[7:0] — idle time, normal op */
#define REG_IDLE_TIME_H        0x66  /* R_IDLE_TIME[15:8] */
#define REG_STEP_S1_L          0x67  /* V0.8: R_IDLE_TIME_SLEEP_1[7:0] */
#define REG_STEP_S1_H          0x68  /* V0.8: R_IDLE_TIME_SLEEP_1[15:8] */
#define REG_STEP_S2_L          0x69  /* V0.8: R_IDLE_TIME_SLEEP_2[7:0] */
#define REG_STEP_S2_H          0x6A  /* V0.8: R_IDLE_TIME_SLEEP_2[15:8] */
#define REG_STEP_OPS1_L        0x6B  /* V0.8: R_Obj_TIME_1[7:0] — вход в слабый сон */
#define REG_STEP_OPS1_H        0x6C  /* V0.8: R_Obj_TIME_1[15:8] */
#define REG_STEP_S1S2_L        0x6D  /* V0.8: R_Obj_TIME_2[7:0] — вход в глубокий сон */
#define REG_STEP_S1S2_H        0x6E  /* V0.8: R_Obj_TIME_2[15:8] */

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

/* Антагонистические пары жестов: обе части одной пары означают, что датчик
 * не смог определить направление (например UP и DOWN одновременно). Такой
 * набор битов — признак неуверенной классификации, а не жеста. */
#define GES_PAIR_UP_DOWN       0x03  /* GES_UP | GES_DOWN */
#define GES_PAIR_LEFT_RIGHT    0x0C  /* GES_LEFT | GES_RIGHT */
#define GES_PAIR_FWD_BACK      0x30  /* GES_FORWARD | GES_BACKWARD */
#define GES_PAIR_ROTATE        0xC0  /* GES_CW | GES_CCW */

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
