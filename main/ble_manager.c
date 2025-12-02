#include "ble_manager.h"

#include <stdio.h>
#include <string.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_bt.h"
#include "esp_nimble_hci.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "os/os_mbuf.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"



static const char *TAG = "BLE";

static ble_notification_callback_t s_notification_cb = NULL;
static ble_control_callback_t s_control_cb = NULL;

static uint8_t notification_value_buf[160];
static uint8_t control_value_buf[48];

static SemaphoreHandle_t s_notif_mutex;
static ble_notification_t s_last_notification = {0};
static char s_last_command[48] = {0};

static uint16_t s_notification_handle;
static uint16_t s_control_handle;
static uint8_t s_addr_type;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static bool s_ble_connected = false;
static bool s_ble_advertising = false;

static const ble_uuid128_t SMARTWATCH_SERVICE_UUID =
    BLE_UUID128_INIT(0x8d, 0x17, 0x6a, 0x59, 0x10, 0x6f, 0x4b, 0x16,
                     0x9f, 0x36, 0x31, 0xe9, 0x3d, 0x50, 0x8a, 0x1d);

static const ble_uuid128_t NOTIFICATION_CHAR_UUID =
    BLE_UUID128_INIT(0xb8, 0xda, 0x9b, 0x5a, 0x12, 0xe4, 0x48, 0x0f,
                     0xa5, 0x9f, 0x07, 0x4f, 0x7d, 0x75, 0x80, 0x24);

static const ble_uuid128_t CONTROL_CHAR_UUID =
    BLE_UUID128_INIT(0x6e, 0xf6, 0x4d, 0x83, 0xa7, 0x9d, 0x45, 0x0b,
                     0xba, 0x29, 0x0c, 0x41, 0x5e, 0xb7, 0x1c, 0xb3);

static int gatt_svr_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                               struct ble_gatt_access_ctxt *ctxt, void *arg);
static void ble_app_advertise(void);
static void ble_on_gatt_register(struct ble_gatt_register_ctxt *ctxt, void *arg);
static bool copy_mbuf_to_buffer(struct os_mbuf *om, uint8_t *dst, uint16_t dst_size,
                                uint16_t *out_len);
static void send_write_ack(uint16_t conn_handle, uint16_t attr_handle, const uint8_t *data,
                           uint16_t len);

static void store_notification(const char *title, const char *body) {
    if (s_notif_mutex) {
        xSemaphoreTake(s_notif_mutex, portMAX_DELAY);
    }

    memset(&s_last_notification, 0, sizeof(s_last_notification));
    strncpy(s_last_notification.title, title ? title : "", sizeof(s_last_notification.title) - 1);
    strncpy(s_last_notification.body, body ? body : "", sizeof(s_last_notification.body) - 1);
    s_last_notification.has_data = true;
    s_last_notification.has_unread = true;

    if (s_notif_mutex) {
        xSemaphoreGive(s_notif_mutex);
    }

    if (s_notification_cb) {
        s_notification_cb(s_last_notification.title, s_last_notification.body);
    }
}

static void handle_notification_write(const uint8_t *data, uint16_t len) {
    char buffer[sizeof(s_last_notification.title) + sizeof(s_last_notification.body) + 4] = {0};
    if (len >= sizeof(buffer)) {
        len = sizeof(buffer) - 1;
    }
    memcpy(buffer, data, len);
    buffer[len] = '\0';

    char *title = buffer;
    char *body = NULL;

    char *newline = strchr(buffer, '\n');
    char *pipe_sep = strchr(buffer, '|');
    char *sep = newline ? newline : pipe_sep;
    if (sep) {
        *sep = '\0';
        body = sep + 1;
    }

    if (!body) {
        body = buffer;
        title = "Bildirim";
    }

    store_notification(title, body);
    ESP_LOGI(TAG, "Notification received: title='%s' body='%s'", title, body);
}

static void handle_control_write(const uint8_t *data, uint16_t len) {
    memset(s_last_command, 0, sizeof(s_last_command));
    if (len >= sizeof(s_last_command)) {
        len = sizeof(s_last_command) - 1;
    }
    memcpy(s_last_command, data, len);
    s_last_command[len] = '\0';

    ESP_LOGI(TAG, "Control command received: %s", s_last_command);
    if (s_control_cb) {
        s_control_cb(s_last_command);
    }
}

static bool copy_mbuf_to_buffer(struct os_mbuf *om, uint8_t *dst, uint16_t dst_size,
                                uint16_t *out_len) {
    int rc = ble_hs_mbuf_to_flat(om, dst, dst_size - 1, out_len);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to copy GATT write payload; rc=%d", rc);
        return false;
    }

    if (*out_len >= dst_size) {
        *out_len = dst_size - 1;
    }
    dst[*out_len] = '\0';
    return true;
}

/* ULTRA-VERBOSE gatt_svr_chr_access callback */
static int gatt_svr_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                               struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)arg;
    
    // Log every single access
    printf("GATT CALLBACK INVOKED - printf check\n");
    fflush(stdout);
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔════════════════════════════════════════════════╗");
    ESP_LOGI(TAG, "║   GATT CALLBACK INVOKED - TIMESTAMP: %lld     ║", esp_timer_get_time());
    ESP_LOGI(TAG, "╚════════════════════════════════════════════════╝");
    
    char uuid_str[BLE_UUID_STR_LEN];
    const char *uuid_readable = ctxt->chr ? ble_uuid_to_str(ctxt->chr->uuid, uuid_str) : "(null)";
    uint16_t pkt_len = ctxt->om ? OS_MBUF_PKTLEN(ctxt->om) : 0;
    
    ESP_LOGI(TAG, "  Operation: %d (0=read_chr, 1=write_chr, 2=read_dsc, 3=write_dsc)", ctxt->op);
    ESP_LOGI(TAG, "  Conn Handle: %u", conn_handle);
    ESP_LOGI(TAG, "  Attr Handle: %u", attr_handle);
    ESP_LOGI(TAG, "  UUID: %s", uuid_readable);
    ESP_LOGI(TAG, "  Packet Len: %u", pkt_len);
    ESP_LOGI(TAG, "  s_notification_handle: %u", s_notification_handle);
    ESP_LOGI(TAG, "  s_control_handle: %u", s_control_handle);

    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        printf("WRITE OPERATION DETECTED - printf check\n");
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "  ▶▶▶▶▶ WRITE OPERATION DETECTED ◀◀◀◀◀");
        ESP_LOGI(TAG, "");
        
        uint8_t buffer[256] = {0};
        uint16_t data_len = 0;

        if (!copy_mbuf_to_buffer(ctxt->om, buffer, sizeof(buffer), &data_len)) {
            ESP_LOGE(TAG, "  ✗ Failed to copy mbuf data");
            return BLE_ATT_ERR_UNLIKELY;
        }

        ESP_LOGI(TAG, "  Write Data Length: %u bytes", data_len);
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, buffer, data_len, ESP_LOG_INFO);
        ESP_LOGI(TAG, "  Write Data (ASCII): '%.*s'", data_len, buffer);

        if (attr_handle == s_notification_handle) {
            ESP_LOGI(TAG, "  ✓✓✓ NOTIFICATION CHARACTERISTIC WRITE ✓✓✓");
            handle_notification_write(buffer, data_len);
            send_write_ack(conn_handle, s_notification_handle, buffer, data_len);
            return 0;
        }

        if (attr_handle == s_control_handle) {
            ESP_LOGI(TAG, "  ✓✓✓ CONTROL CHARACTERISTIC WRITE ✓✓✓");
            handle_control_write(buffer, data_len);
            send_write_ack(conn_handle, s_control_handle, buffer, data_len);
            return 0;
        }

        ESP_LOGW(TAG, "  ✗✗✗ UNKNOWN HANDLE - NO MATCH ✗✗✗");
        return BLE_ATT_ERR_UNLIKELY;
    }

    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        ESP_LOGI(TAG, "  ▶▶▶ READ OPERATION ◀◀◀");
        
        if (attr_handle == s_notification_handle) {
            ESP_LOGI(TAG, "  Reading notification characteristic");
            int rc = os_mbuf_append(ctxt->om, &s_last_notification, sizeof(s_last_notification));
            return (rc == 0) ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        }

        if (attr_handle == s_control_handle) {
            ESP_LOGI(TAG, "  Reading control characteristic");
            int rc = os_mbuf_append(ctxt->om, s_last_command, strlen(s_last_command));
            return (rc == 0) ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        }

        ESP_LOGW(TAG, "  Read from unknown handle");
        return BLE_ATT_ERR_ATTR_NOT_FOUND;
    }

    ESP_LOGW(TAG, "  Unhandled operation type");
    return BLE_ATT_ERR_UNLIKELY;
}

static void ble_on_gatt_register(struct ble_gatt_register_ctxt *ctxt, void *arg) {
    char uuid_str[BLE_UUID_STR_LEN];

    switch (ctxt->op) {
        case BLE_GATT_REGISTER_OP_SVC:
            ESP_LOGI(TAG, "Registered service %s with handle=%u",
                     ble_uuid_to_str(ctxt->svc.svc_def->uuid, uuid_str), ctxt->svc.handle);
            break;
        case BLE_GATT_REGISTER_OP_CHR:
            ESP_LOGI(TAG, "Registered characteristic %s with def_handle=%u val_handle=%u",
                     ble_uuid_to_str(ctxt->chr.chr_def->uuid, uuid_str), 
                     ctxt->chr.def_handle, ctxt->chr.val_handle);
            break;
        case BLE_GATT_REGISTER_OP_DSC:
            ESP_LOGI(TAG, "Registered descriptor %s with handle=%u",
                     ble_uuid_to_str(ctxt->dsc.dsc_def->uuid, uuid_str), ctxt->dsc.handle);
            break;
        default:
            ESP_LOGW(TAG, "Unknown GATT registration op=%d", ctxt->op);
            break;
    }
}


static const struct ble_gatt_svc_def gatt_svr_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &SMARTWATCH_SERVICE_UUID.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid = &NOTIFICATION_CHAR_UUID.u,
                .access_cb = gatt_svr_chr_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_notification_handle,
            },
            {
                .uuid = &CONTROL_CHAR_UUID.u,
                .access_cb = gatt_svr_chr_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_control_handle,
            },
            {0},
        },
    },
    {0},
};
static int ble_gap_event(struct ble_gap_event *event, void *arg) {
    ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
    ESP_LOGI(TAG, "║   BLE GAP EVENT: type=%d          ║", event->type);
    ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
    
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                ESP_LOGI(TAG, "BLE connected");
                s_ble_connected = true;
                s_conn_handle = event->connect.conn_handle;
                s_ble_advertising = false;
            } else {
                ESP_LOGW(TAG, "BLE connect failed; status=%d", event->connect.status);
                ble_app_advertise();
            }
            break;
        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGI(TAG, "BLE disconnected; reason=%d", event->disconnect.reason);
            s_ble_connected = false;
            s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            ble_app_advertise();
            break;
        case BLE_GAP_EVENT_ADV_COMPLETE:
            ESP_LOGI(TAG, "Advertisement complete; restarting");
            s_ble_advertising = false;
            ble_app_advertise();
            break;
        case BLE_GAP_EVENT_SUBSCRIBE:
            ESP_LOGI(TAG, "Subscription event; attr_handle=%u reason=%d prevn=%d curn=%d previ=%d curi=%d",
                     event->subscribe.attr_handle, event->subscribe.reason, event->subscribe.prev_notify,
                     event->subscribe.cur_notify, event->subscribe.prev_indicate, event->subscribe.cur_indicate);
            break;
        case BLE_GAP_EVENT_MTU:
            ESP_LOGI(TAG, "MTU update event; conn_handle=%d cid=%d mtu=%d",
                     event->mtu.conn_handle, event->mtu.channel_id, event->mtu.value);
            break;
        case BLE_GAP_EVENT_NOTIFY_TX:
            ESP_LOGI(TAG, "Notify TX complete; status=%d conn_handle=%d attr_handle=%d",
                     event->notify_tx.status, event->notify_tx.conn_handle, event->notify_tx.attr_handle);
            break;
        default:
            ESP_LOGI(TAG, "Unhandled GAP event: %d", event->type);
            break;
    }
    return 0;
}

static void send_write_ack(uint16_t conn_handle, uint16_t attr_handle, const uint8_t *data,
                           uint16_t len) {
    if (conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return;
    }

    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
    if (!om) {
        ESP_LOGE(TAG, "Failed to alloc mbuf for write ack");
        return;
    }

    int rc = ble_gatts_notify_custom(conn_handle, attr_handle, om);
    if (rc != 0) {
        ESP_LOGW(TAG, "Failed to send write ack (attr=%u rc=%d)", attr_handle, rc);
    }
}


static void ble_app_advertise(void) {
    struct ble_gap_adv_params adv_params = {0};
    struct ble_hs_adv_fields adv_fields = {0};
    struct ble_hs_adv_fields rsp_fields = {0};

    adv_fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    adv_fields.tx_pwr_lvl_is_present = 1;
    adv_fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    adv_fields.uuids128 = (const ble_uuid128_t *)&SMARTWATCH_SERVICE_UUID;
    adv_fields.num_uuids128 = 1;
    adv_fields.uuids128_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&adv_fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to set advertisement data; rc=%d", rc);
        return;
    }

    rsp_fields.name = (uint8_t *)ble_svc_gap_device_name();
    rsp_fields.name_len = strlen(ble_svc_gap_device_name());
    rsp_fields.name_is_complete = 1;

    rc = ble_gap_adv_rsp_set_fields(&rsp_fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to set scan response data; rc=%d", rc);
        return;
    }

    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(s_addr_type, NULL, BLE_HS_FOREVER, &adv_params, ble_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to start advertising; rc=%d", rc);
    } else {
        s_ble_advertising = true;
        ESP_LOGI(TAG, "Advertising started (addr_type=%u) - VERSION 2", s_addr_type);
    }
}

static void ble_on_sync(void) {
    int rc = ble_hs_id_infer_auto(0, &s_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to infer address type; rc=%d", rc);
        return;
    }

    rc = ble_gatts_start();
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to start GATT services; rc=%d", rc);
        return;
    }

    ESP_LOGI(TAG, "GATT services started; notification handle=%u control handle=%u", s_notification_handle,
             s_control_handle);

    ble_app_advertise();
}

static void ble_on_reset(int reason) {
    ESP_LOGW(TAG, "BLE stack reset; reason=%d", reason);
}

static void ble_host_task(void *param) {
    ESP_LOGI(TAG, "BLE Host Task Started");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t ble_manager_init(ble_notification_callback_t notification_cb,
                           ble_control_callback_t control_cb) {
    s_notification_cb = notification_cb;
    s_control_cb = control_cb;
    ble_hs_cfg.gatts_register_cb = ble_on_gatt_register;
    
    // Enable verbose NimBLE logging
    esp_log_level_set("NimBLE", ESP_LOG_DEBUG);
    esp_log_level_set("BLE_GATTS", ESP_LOG_DEBUG);
    esp_log_level_set("BLE_HS", ESP_LOG_DEBUG);
    s_notif_mutex = xSemaphoreCreateMutex();

    esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);

    // esp_nimble_hci_and_controller_init() is removed in IDF v5.0+
    // nimble_port_init() handles the controller init if configured.

    esp_err_t ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init NimBLE host: %s", esp_err_to_name(ret));
        return ret;
    }

    // Security Configuration
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 0; // Legacy pairing
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_svc_gap_device_name_set("SmartWatch BLE");
    ble_svc_gap_init();
    ble_svc_gatt_init();

    ble_gatts_count_cfg(gatt_svr_svcs);
    ret = ble_gatts_add_svcs(gatt_svr_svcs);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add GATT services: %s", esp_err_to_name(ret));
        return ret;
    }

    ble_hs_cfg.reset_cb = ble_on_reset;
    ble_hs_cfg.sync_cb = ble_on_sync;
    ble_hs_cfg.gatts_register_cb = ble_on_gatt_register;

    nimble_port_freertos_init(ble_host_task);

    ESP_LOGI(TAG, "BLE manager initialized and advertising");
    return ESP_OK;
}

bool ble_manager_get_last_notification(ble_notification_t *out, bool clear_unread) {
    if (!out) return false;

    if (s_notif_mutex) {
        xSemaphoreTake(s_notif_mutex, portMAX_DELAY);
    }
    *out = s_last_notification;
    if (clear_unread) {
        s_last_notification.has_unread = false;
    }
    if (s_notif_mutex) {
        xSemaphoreGive(s_notif_mutex);
    }

    return out->has_data;
}

bool ble_manager_has_unread_notification(void) {
    bool unread = false;
    if (s_notif_mutex) {
        xSemaphoreTake(s_notif_mutex, portMAX_DELAY);
    }
    unread = s_last_notification.has_unread;
    if (s_notif_mutex) {
        xSemaphoreGive(s_notif_mutex);
    }
    return unread;
}

void ble_manager_mark_notifications_read(void) {
    if (s_notif_mutex) {
        xSemaphoreTake(s_notif_mutex, portMAX_DELAY);
    }
    s_last_notification.has_unread = false;
    if (s_notif_mutex) {
        xSemaphoreGive(s_notif_mutex);
    }
}

bool ble_manager_is_connected(void) {
    return s_ble_connected;
}

bool ble_manager_is_active(void) {
    return s_ble_connected || s_ble_advertising;
}
