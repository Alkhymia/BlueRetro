/*
 * Copyright (c) 2019-2020, Jacques Gagnon
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include "../zephyr/types.h"
#include "../util.h"
#include "xb1.h"

/* xinput buttons */
enum {
    XB1_A = 0,
    XB1_B,
    XB1_X,
    XB1_Y,
    XB1_LB,
    XB1_RB,
    XB1_VIEW,
    XB1_MENU,
    XB1_LJ,
    XB1_RJ,
};

/* dinput buttons */
enum {
    XB1_DI_A = 0,
    XB1_DI_B,
    XB1_DI_X = 3,
    XB1_DI_Y,
    XB1_DI_LB = 6,
    XB1_DI_RB,
    XB1_DI_MENU = 11,
    XB1_DI_LJ = 13,
    XB1_DI_RJ,
    XB1_DI_VIEW = 16,
};

/* report 2 */
enum {
    XB1_XBOX = 0,
};

/* adaptive extra */
enum {
    XB1_ADAPTIVE_X1 = 0,
    XB1_ADAPTIVE_X2,
    XB1_ADAPTIVE_X3,
    XB1_ADAPTIVE_X4,
};

const uint8_t xb1_axes_idx[ADAPTER_MAX_AXES] =
{
/*  AXIS_LX, AXIS_LY, AXIS_RX, AXIS_RY, TRIG_L, TRIG_R  */
    0,       1,       2,       3,       4,      5
};

const struct ctrl_meta xb1_btn_meta =
{
    .polarity = 0,
};

const struct ctrl_meta xb1_axes_meta[ADAPTER_MAX_AXES] =
{
    {.neutral = 0x8000, .abs_max = 0x8000},
    {.neutral = 0x8000, .abs_max = 0x8000, .polarity = 1},
    {.neutral = 0x8000, .abs_max = 0x8000},
    {.neutral = 0x8000, .abs_max = 0x8000, .polarity = 1},
    {.neutral = 0x0000, .abs_max = 0x3FF},
    {.neutral = 0x0000, .abs_max = 0x3FF},
};

struct xb1_map {
    uint16_t axes[6];
    uint8_t hat;
    uint32_t buttons; /* include view for dinput */
    uint8_t pad[15]; /* adaptive controller unmap */
    uint8_t extra; /* adaptive extra buttons */
} __packed;

struct xb1_rumble {
    uint8_t enable;
    uint8_t mag_lt;
    uint8_t mag_rt;
    uint8_t mag_l;
    uint8_t mag_r;
    uint8_t duration;
    uint8_t delay;
    uint8_t cnt;
} __packed;

const struct xb1_rumble xb1_rumble_on = {
    .enable = 0x03,
    .mag_l = 0x1e,
    .mag_r = 0x1e,
    .duration = 0xFF,
    .cnt = 0x00,
};

const struct xb1_rumble xb1_rumble_off = {
    .enable = 0x03,
    .mag_l = 0x00,
    .mag_r = 0x00,
    .duration = 0xFF,
    .cnt = 0xFF,
};

const uint32_t xb1_mask[4] = {0xBB3F0FFF, 0x00000000, 0x00000000, 0x00000000};
const uint32_t xb1_mask2[4] = {0x00400000, 0x00000000, 0x00000000, 0x00000000};
const uint32_t xb1_adaptive_mask[4] = {0xBB3FFFFF, 0x00000000, 0x00000000, 0x00000000};
const uint32_t xb1_desc[4] = {0x110000FF, 0x00000000, 0x00000000, 0x00000000};

const uint32_t xb1_btns_mask[32] = {
    0, 0, 0, 0,
    0, 0, 0, 0,
    0, 0, 0, 0,
    0, 0, 0, 0,
    BIT(XB1_X), BIT(XB1_B), BIT(XB1_A), BIT(XB1_Y),
    BIT(XB1_MENU), BIT(XB1_VIEW), 0, 0,
    0, BIT(XB1_LB), 0, BIT(XB1_LJ),
    0, BIT(XB1_RB), 0, BIT(XB1_RJ),
};

const uint32_t xb1_dinput_btns_mask[32] = {
    0, 0, 0, 0,
    0, 0, 0, 0,
    0, 0, 0, 0,
    0, 0, 0, 0,
    BIT(XB1_DI_X), BIT(XB1_DI_B), BIT(XB1_DI_A), BIT(XB1_DI_Y),
    BIT(XB1_DI_MENU), BIT(XB1_DI_VIEW), 0, 0,
    0, BIT(XB1_DI_LB), 0, BIT(XB1_DI_LJ),
    0, BIT(XB1_DI_RB), 0, BIT(XB1_DI_RJ),
};

const uint32_t xb1_adaptive_btns_mask[32] = {
    0, 0, 0, 0,
    0, 0, 0, 0,
    0, 0, 0, 0,
    BIT(XB1_ADAPTIVE_X4), BIT(XB1_ADAPTIVE_X3), BIT(XB1_ADAPTIVE_X2), BIT(XB1_ADAPTIVE_X1),
    0, 0, 0, 0,
    0, 0, 0, 0,
    0, 0, 0, 0,
    0, 0, 0, 0,
};

void xb1_to_generic(struct bt_data *bt_data, struct generic_ctrl *ctrl_data) {
    struct xb1_map *map = (struct xb1_map *)bt_data->input;

    memset((void *)ctrl_data, 0, sizeof(*ctrl_data));

    ctrl_data->desc = (uint32_t *)xb1_desc;

    if (bt_data->report_id == 0x01) {
        const uint32_t *btns_mask;

        if (bt_data->dev_type == XB1_ADAPTIVE) {
            ctrl_data->mask = (uint32_t *)xb1_adaptive_mask;
            btns_mask = xb1_dinput_btns_mask;

            for (uint32_t i = 0; i < ARRAY_SIZE(generic_btns_mask); i++) {
                if (map->extra & xb1_adaptive_btns_mask[i]) {
                    ctrl_data->btns[0].value |= generic_btns_mask[i];
                }
            }
        }
        else {
            ctrl_data->mask = (uint32_t *)xb1_mask;
            btns_mask = xb1_btns_mask;
        }

        for (uint32_t i = 0; i < ARRAY_SIZE(generic_btns_mask); i++) {
            if (map->buttons & btns_mask[i]) {
                ctrl_data->btns[0].value |= generic_btns_mask[i];
            }
        }

        /* Convert hat to regular btns */
        ctrl_data->btns[0].value |= hat_to_ld_btns[(map->hat - 1) & 0xF];

        if (!atomic_test_bit(&bt_data->flags, BT_INIT)) {
            for (uint32_t i = 0; i < ADAPTER_MAX_AXES; i++) {
                bt_data->axes_cal[i] = -(map->axes[xb1_axes_idx[i]] - xb1_axes_meta[i].neutral);
            }
            atomic_set_bit(&bt_data->flags, BT_INIT);
        }

        for (uint32_t i = 0; i < ADAPTER_MAX_AXES; i++) {
            ctrl_data->axes[i].meta = &xb1_axes_meta[i];
            ctrl_data->axes[i].value = map->axes[xb1_axes_idx[i]] - xb1_axes_meta[i].neutral + bt_data->axes_cal[i];
        }
    }
    else if (bt_data->report_id == 0x02) {
        ctrl_data->mask = (uint32_t *)xb1_mask2;

        if (bt_data->input[0] & BIT(XB1_XBOX)) {
            ctrl_data->btns[0].value |= BIT(PAD_MT);
        }
    }
}

void xb1_fb_from_generic(struct generic_fb *fb_data, struct bt_data *bt_data) {
    struct xb1_rumble *rumble = (struct xb1_rumble *)bt_data->output;

    if (fb_data->state) {
        memcpy((void *)rumble, (void *)&xb1_rumble_on, sizeof(xb1_rumble_on));
    }
    else {
        memcpy((void *)rumble, (void *)&xb1_rumble_off, sizeof(xb1_rumble_off));
    }
}
