/*
 * Copyright (c) 2019-2020, Jacques Gagnon
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <sys/stat.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/ringbuf.h>
#include <esp_system.h>
#include <esp_bt.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include "host.h"
#include "hci.h"
#include "l2cap.h"
#include "sdp.h"
#include "att.h"
#include "../util.h"

//#define H4_TRACE /* Display packet dump that can be parsed by wireshark/text2pcap */

#define BT_TX 0
#define BT_RX 1
#define BT_DEV_MAX 7

#define LINK_KEYS_FILE "/sd/linkkeys.bin"
#define BDADDR_FILE "/sd/bdaddr.bin"

enum {
    /* BT CTRL flags */
    BT_CTRL_READY,
    BT_HOST_DISCONN_SW_INHIBIT,
};

struct bt_host_link_keys {
    uint32_t index;
    struct bt_hci_evt_link_key_notify link_keys[16];
} __packed;

struct bt_hci_pkt bt_hci_pkt_tmp;

static struct bt_host_link_keys bt_host_link_keys = {0};
static RingbufHandle_t txq_hdl;
static struct bt_dev bt_dev_conf = {0};
static struct bt_dev bt_dev[BT_DEV_MAX] = {0};
static atomic_t bt_flags = 0;
static uint32_t frag_size = 0;
static uint32_t frag_offset = 0;
static uint8_t frag_buf[1024];
static esp_timer_handle_t disconn_sw_timer_hdl;

#ifdef H4_TRACE
static void bt_h4_trace(uint8_t *data, uint16_t len, uint8_t dir);
#endif /* H4_TRACE */
static int32_t bt_host_load_bdaddr_from_file(void);
static int32_t bt_host_load_keys_from_file(struct bt_host_link_keys *data);
static int32_t bt_host_store_keys_on_file(struct bt_host_link_keys *data);
static void bt_host_acl_hdlr(struct bt_hci_pkt *bt_hci_acl_pkt, uint32_t len);
static void bt_host_tx_pkt_ready(void);
static int bt_host_rx_pkt(uint8_t *data, uint16_t len);
static void bt_host_task(void *param);

static esp_vhci_host_callback_t vhci_host_cb = {
    bt_host_tx_pkt_ready,
    bt_host_rx_pkt
};

#ifdef H4_TRACE
static void bt_h4_trace(uint8_t *data, uint16_t len, uint8_t dir) {
    uint8_t col;
    uint16_t byte, line;
    uint16_t line_max = len/16;

    if (len % 16)
        line_max++;

    if (dir)
        printf("I ");
    else
        printf("O ");

    for (byte = 0, line = 0; line < line_max; line++) {
        printf("%06X", byte);
        for (col = 0; col < 16 && byte < len; col++, byte++) {
            printf(" %02X", data[byte]);
        }
        printf("\n");
    }
}
#endif /* H4_TRACE */

static void bt_host_disconn_sw_callback(void *arg) {
    printf("# %s\n", __FUNCTION__);

    esp_timer_delete(disconn_sw_timer_hdl);
    disconn_sw_timer_hdl = NULL;

    atomic_clear_bit(&bt_flags, BT_HOST_DISCONN_SW_INHIBIT);
}

static int32_t bt_host_load_bdaddr_from_file(void) {
    struct stat st;
    int32_t ret = -1;

    if (stat(BDADDR_FILE, &st) != 0) {
        printf("%s: No BDADDR on SD. Using ESP32's MAC\n", __FUNCTION__);
    }
    else {
        FILE *file = fopen(BDADDR_FILE, "rb");
        if (file == NULL) {
            printf("%s: failed to open file for reading\n", __FUNCTION__);
        }
        else {
            uint8_t test_mac[6];
            fread((void *)test_mac, sizeof(test_mac), 1, file);
            fclose(file);
            test_mac[5] -= 2; /* Set base mac to BDADDR-2 so that BDADDR end up what we want */
            esp_base_mac_addr_set(test_mac);
            printf("%s: Using BDADDR.BIN MAC\n", __FUNCTION__);
            ret = 0;
        }
    }
    return ret;
}

static int32_t bt_host_load_keys_from_file(struct bt_host_link_keys *data) {
    struct stat st;
    int32_t ret = -1;

    if (stat(LINK_KEYS_FILE, &st) != 0) {
        printf("%s: No link keys on SD. Creating...\n", __FUNCTION__);
        ret = bt_host_store_keys_on_file(data);
    }
    else {
        FILE *file = fopen(LINK_KEYS_FILE, "rb");
        if (file == NULL) {
            printf("%s: failed to open file for reading\n", __FUNCTION__);
        }
        else {
            fread((void *)data, sizeof(*data), 1, file);
            fclose(file);
            ret = 0;
        }
    }
    return ret;
}

static int32_t bt_host_store_keys_on_file(struct bt_host_link_keys *data) {
    int32_t ret = -1;

    FILE *file = fopen(LINK_KEYS_FILE, "wb");
    if (file == NULL) {
        printf("%s: failed to open file for writing\n", __FUNCTION__);
    }
    else {
        fwrite((void *)data, sizeof(*data), 1, file);
        fclose(file);
        ret = 0;
    }
    return ret;
}

static void bt_tx_task(void *param) {
    size_t packet_len;
    uint8_t *packet;

    while(1) {
        /* TX packet from Q */
        if (atomic_test_bit(&bt_flags, BT_CTRL_READY)) {
            packet = (uint8_t *)xRingbufferReceive(txq_hdl, &packet_len, portMAX_DELAY);
            if (packet) {
                if (packet[0] == 0xFF) {
                    /* Internal wait packet */
                    vTaskDelay(packet[1] / portTICK_PERIOD_MS);
                }
                else {
#ifdef H4_TRACE
                    bt_h4_trace(packet, packet_len, BT_TX);
#endif /* H4_TRACE */
                    atomic_clear_bit(&bt_flags, BT_CTRL_READY);
                    esp_vhci_host_send_packet(packet, packet_len);
                }
                vRingbufferReturnItem(txq_hdl, (void *)packet);
            }
        }
    }
}

static void bt_fb_task(void *param) {
    size_t fb_len;
    uint8_t *fb_data;

    while(1) {
        /* Look for rumble/led feedback data */
        fb_data = (uint8_t *)xRingbufferReceive(wired_adapter.input_q_hdl, &fb_len, portMAX_DELAY);
        if (fb_data) {
            struct bt_dev *device = &bt_dev[fb_data[0]];
            if (adapter_bridge_fb(fb_data, fb_len, &bt_adapter.data[device->id])) {
                bt_hid_feedback(device, bt_adapter.data[device->id].output);
            }
            vRingbufferReturnItem(wired_adapter.input_q_hdl, (void *)fb_data);
        }
    }
}

static void bt_host_task(void *param) {
    while(1) {
        /* Disconnect all devices on BOOT switch press */
        if (!gpio_get_level(0) && !atomic_test_bit(&bt_flags, BT_HOST_DISCONN_SW_INHIBIT)) {
            const esp_timer_create_args_t disconn_sw_timer_args = {
                .callback = &bt_host_disconn_sw_callback,
                .arg = NULL,
                .name = "disconn_sw_timer"
            };

            atomic_set_bit(&bt_flags, BT_HOST_DISCONN_SW_INHIBIT);
            printf("# %s BOOT SW pressed, DISCONN all devices!\n", __FUNCTION__);
            for (uint32_t i = 0; i < BT_DEV_MAX; i++) {
                if (atomic_test_bit(&bt_dev[i].flags, BT_DEV_DEVICE_FOUND)) {
                    bt_hci_disconnect(&bt_dev[i]);
                }
            }

            /* Inhibit SW press for 2 seconds */
            esp_timer_create(&disconn_sw_timer_args, (esp_timer_handle_t *)&disconn_sw_timer_hdl);
            esp_timer_start_once(disconn_sw_timer_hdl, 2000000);
        }
        /* Per device housekeeping */
        for (uint32_t i = 0; i < BT_DEV_MAX; i++) {
            if (atomic_test_bit(&bt_dev[i].flags, BT_DEV_DEVICE_FOUND)) {
                if (atomic_test_bit(&bt_dev[i].flags, BT_DEV_SDP_DATA)) {
                    bt_sdp_parser(&bt_adapter.data[i]);
                    if (bt_adapter.data[i].dev_type != bt_dev[i].type) {
                        bt_dev[i].type = bt_adapter.data[i].dev_type;
                        if (atomic_test_bit(&bt_dev[i].flags, BT_DEV_HID_INTR_READY)) {
                            bt_hid_init(&bt_dev[i]);
                        }
                    }
                    atomic_clear_bit(&bt_dev[i].flags, BT_DEV_SDP_DATA);
                }
            }
        }
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}

static void bt_host_acl_hdlr(struct bt_hci_pkt *bt_hci_acl_pkt, uint32_t len) {
    struct bt_dev *device = NULL;
    struct bt_hci_pkt *pkt = bt_hci_acl_pkt;
    uint32_t pkt_len = len;
    bt_host_get_dev_from_handle(pkt->acl_hdr.handle, &device);

    if (bt_acl_flags(pkt->acl_hdr.handle) == BT_ACL_CONT) {
        memcpy(frag_buf + frag_offset, (void *)pkt + BT_HCI_H4_HDR_SIZE + BT_HCI_ACL_HDR_SIZE,
            pkt->acl_hdr.len);
        frag_offset += pkt->acl_hdr.len;
        if (frag_offset < frag_size) {
            printf("# %s Waiting for next fragment. offset: %d size %d\n", __FUNCTION__, frag_offset, frag_size);
            return;
        }
        pkt = (struct bt_hci_pkt *)frag_buf;
        pkt_len = frag_size;
        printf("# %s process reassembled frame. offset: %d size %d\n", __FUNCTION__, frag_offset, frag_size);
    }
    if (bt_acl_flags(pkt->acl_hdr.handle) == BT_ACL_START
        && (pkt_len - (BT_HCI_H4_HDR_SIZE + BT_HCI_ACL_HDR_SIZE + sizeof(struct bt_l2cap_hdr))) < pkt->l2cap_hdr.len) {
        memcpy(frag_buf, (void *)pkt, pkt_len);
        frag_offset = pkt_len;
        frag_size = pkt->l2cap_hdr.len + BT_HCI_H4_HDR_SIZE + BT_HCI_ACL_HDR_SIZE + sizeof(struct bt_l2cap_hdr);
        printf("# %s Detected fragmented frame start\n", __FUNCTION__);
        return;
    }

    if (device == NULL) {
        if (pkt->l2cap_hdr.cid == BT_L2CAP_CID_ATT) {
            bt_att_hdlr(&bt_dev_conf, pkt, pkt_len);
        }
        else {
            printf("# %s dev NULL!\n", __FUNCTION__);
        }
        return;
    }

    if (pkt->l2cap_hdr.cid == BT_L2CAP_CID_BR_SIG) {
        bt_l2cap_sig_hdlr(device, pkt);
    }
    else if (pkt->l2cap_hdr.cid == device->sdp_tx_chan.scid ||
        pkt->l2cap_hdr.cid == device->sdp_rx_chan.scid) {
        bt_sdp_hdlr(device, pkt);
    }
    else if (pkt->l2cap_hdr.cid == device->ctrl_chan.scid ||
        pkt->l2cap_hdr.cid == device->intr_chan.scid) {
        bt_hid_hdlr(device, pkt);
    }
}

/*
 * @brief: BT controller callback function, used to notify the upper layer that
 *         controller is ready to receive command
 */
static void bt_host_tx_pkt_ready(void) {
    atomic_set_bit(&bt_flags, BT_CTRL_READY);
}

/*
 * @brief: BT controller callback function, to transfer data packet to upper
 *         controller is ready to receive command
 */
static int bt_host_rx_pkt(uint8_t *data, uint16_t len) {
    struct bt_hci_pkt *bt_hci_pkt = (struct bt_hci_pkt *)data;
#ifdef H4_TRACE
    bt_h4_trace(data, len, BT_RX);
#endif /* H4_TRACE */

    switch(bt_hci_pkt->h4_hdr.type) {
        case BT_HCI_H4_TYPE_ACL:
            bt_host_acl_hdlr(bt_hci_pkt, len);
            break;
        case BT_HCI_H4_TYPE_EVT:
            bt_hci_evt_hdlr(bt_hci_pkt);
            break;
        default:
            printf("# %s unsupported packet type: 0x%02X\n", __FUNCTION__, bt_hci_pkt->h4_hdr.type);
            break;
    }

    return 0;
}

int32_t bt_host_get_new_dev(struct bt_dev **device) {
    for (uint32_t i = 0; i < BT_DEV_MAX; i++) {
        if (!atomic_test_bit(&bt_dev[i].flags, BT_DEV_DEVICE_FOUND)) {
            *device = &bt_dev[i];
            return i;
        }
    }
    return -1;
}

int32_t bt_host_get_active_dev(struct bt_dev **device) {
    for (uint32_t i = 0; i < BT_DEV_MAX; i++) {
        if (atomic_test_bit(&bt_dev[i].flags, BT_DEV_DEVICE_FOUND)) {
            *device = &bt_dev[i];
            return i;
        }
    }
    return -1;
}

int32_t bt_host_get_dev_from_bdaddr(uint8_t *bdaddr, struct bt_dev **device) {
    for (uint32_t i = 0; i < BT_DEV_MAX; i++) {
        if (atomic_test_bit(&bt_dev[i].flags, BT_DEV_DEVICE_FOUND) && memcmp((void *)bdaddr, bt_dev[i].remote_bdaddr, 6) == 0) {
            *device = &bt_dev[i];
            return i;
        }
    }
    return -1;
}

int32_t bt_host_get_dev_from_handle(uint16_t handle, struct bt_dev **device) {
    for (uint32_t i = 0; i < BT_DEV_MAX; i++) {
        if (atomic_test_bit(&bt_dev[i].flags, BT_DEV_DEVICE_FOUND) && bt_acl_handle(handle) == bt_dev[i].acl_handle) {
            *device = &bt_dev[i];
            return i;
        }
    }
    return -1;
}

int32_t bt_host_get_dev_conf(struct bt_dev **device) {
    *device = &bt_dev_conf;
    return 0;
}

void bt_host_reset_dev(struct bt_dev *device) {
    adapter_init_buffer(device->id);
    memset((void *)&bt_adapter.data[device->id], 0, sizeof(bt_adapter.data[0]));
    memset((void *)device, 0, sizeof(*device));
}

void bt_host_q_wait_pkt(uint32_t ms) {
    uint8_t packet[2] = {0xFF, ms};

    bt_host_txq_add(packet, sizeof(packet));
}

int32_t bt_host_init(void) {
    gpio_config_t io_conf = {0};
    /* Initialize NVS — it is used to store PHY calibration data */
    int32_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* INIT BOOT SW */
    io_conf.intr_type = GPIO_PIN_INTR_DISABLE;
    io_conf.pin_bit_mask = BIT(0);
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io_conf);

    bt_host_load_bdaddr_from_file();

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();

    if ((ret = esp_bt_controller_init(&bt_cfg)) != ESP_OK) {
        printf("Bluetooth controller initialize failed: %s", esp_err_to_name(ret));
        return ret;
    }

    if ((ret = esp_bt_controller_enable(ESP_BT_MODE_BTDM)) != ESP_OK) {
        printf("Bluetooth controller enable failed: %s", esp_err_to_name(ret));
        return ret;
    }

    esp_vhci_host_register_callback(&vhci_host_cb);

    bt_host_tx_pkt_ready();

    txq_hdl = xRingbufferCreate(256*8, RINGBUF_TYPE_NOSPLIT);
    if (txq_hdl == NULL) {
        printf("Failed to create ring buffer\n");
        return ret;
    }

    bt_host_load_keys_from_file(&bt_host_link_keys);

    xTaskCreatePinnedToCore(&bt_host_task, "bt_host_task", 4096, NULL, 5, NULL, 0);
    xTaskCreatePinnedToCore(&bt_fb_task, "bt_fb_task", 2048, NULL, 10, NULL, 0);
    xTaskCreatePinnedToCore(&bt_tx_task, "bt_tx_task", 2048, NULL, 11, NULL, 0);

    bt_hci_init();

    return ret;
}

int32_t bt_host_txq_add(uint8_t *packet, uint32_t packet_len) {
    UBaseType_t ret = xRingbufferSend(txq_hdl, (void *)packet, packet_len, 0);
    if (ret != pdTRUE) {
        printf("# %s txq full!\n", __FUNCTION__);
    }
    return (ret == pdTRUE ? 0 : -1);
}

int32_t bt_host_load_link_key(struct bt_hci_cp_link_key_reply *link_key_reply) {
    int32_t ret = -1;
    for (uint32_t i = 0; i < ARRAY_SIZE(bt_host_link_keys.link_keys); i++) {
        if (memcmp((void *)&link_key_reply->bdaddr, (void *)&bt_host_link_keys.link_keys[i].bdaddr, sizeof(link_key_reply->bdaddr)) == 0) {
            memcpy((void *)link_key_reply->link_key, &bt_host_link_keys.link_keys[i].link_key, sizeof(link_key_reply->link_key));
            ret = 0;
        }
    }
    return ret;
}

int32_t bt_host_store_link_key(struct bt_hci_evt_link_key_notify *link_key_notify) {
    int32_t ret = -1;
    uint32_t index = bt_host_link_keys.index;
    for (uint32_t i = 0; i < ARRAY_SIZE(bt_host_link_keys.link_keys); i++) {
        if (memcmp((void *)&link_key_notify->bdaddr, (void *)&bt_host_link_keys.link_keys[i].bdaddr, sizeof(link_key_notify->bdaddr)) == 0) {
            index = i;
        }
    }
    memcpy((void *)&bt_host_link_keys.link_keys[index], (void *)link_key_notify, sizeof(bt_host_link_keys.link_keys[0]));
    if (index == bt_host_link_keys.index) {
        bt_host_link_keys.index++;
        bt_host_link_keys.index &= 0xF;
    }
    ret = bt_host_store_keys_on_file(&bt_host_link_keys);
    return ret;
}

void bt_host_bridge(struct bt_dev *device, uint8_t report_id, uint8_t *data, uint32_t len) {
    if (device->type == HID_GENERIC) {
        uint32_t i = 0;
        for (; i < REPORT_MAX; i++) {
            if (bt_adapter.data[device->id].reports[i].id == report_id) {
                bt_adapter.data[device->id].report_type = i;
                len = bt_adapter.data[device->id].reports[i].len;
                break;
            }
        }
        if (i == REPORT_MAX) {
            return;
        }
    }
    if (atomic_test_bit(&bt_adapter.data[device->id].flags, BT_INIT) || bt_adapter.data[device->id].report_cnt > 1) {
        bt_adapter.data[device->id].report_id = report_id;
        bt_adapter.data[device->id].dev_id = device->id;
        bt_adapter.data[device->id].dev_type = device->type;
        memcpy(bt_adapter.data[device->id].input, data, len);
        adapter_bridge(&bt_adapter.data[device->id]);
    }
    bt_adapter.data[device->id].report_cnt++;
}
