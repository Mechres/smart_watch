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
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"



static const char *TAG = "BLE";

static ble_notification_callback_t s_notification_cb = NULL;
static ble_control_callback_t s_control_cb = NULL;

static SemaphoreHandle_t s_notif_mutex;
static ble_notification_t s_last_notification = {0};

static uint16_t s_notification_handle;
static uint16_t s_control_handle;
static uint8_t s_addr_type;
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
static int ble_on_gatt_register(struct ble_gatt_register_ctxt *ctxt, void *arg);
static bool copy_mbuf_to_buffer(struct os_mbuf *om, uint8_t *dst, uint16_t dst_size,
                                uint16_t *out_len);

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
    char command[48] = {0};
    if (len >= sizeof(command)) {
        len = sizeof(command) - 1;
    }
    memcpy(command, data, len);
    command[len] = '\0';

    ESP_LOGI(TAG, "Control command received: %s", command);
    if (s_control_cb) {
        s_control_cb(command);
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

static int gatt_svr_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                               struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    uint8_t buffer[sizeof(s_last_notification.title) + sizeof(s_last_notification.body) + 4] = {0};
    uint16_t data_len = 0;

    if (!copy_mbuf_to_buffer(ctxt->om, buffer, sizeof(buffer), &data_len)) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        ESP_LOGW(TAG, "Unhandled GATT op=%d for attr=%u", ctxt->op, attr_handle);
        return BLE_ATT_ERR_UNLIKELY;
    }

    char uuid_str[BLE_UUID_STR_LEN];
    ESP_LOGI(TAG, "GATT write attr=%u uuid=%s len=%u", attr_handle,
             ble_uuid_to_str(ctxt->chr->uuid, uuid_str), data_len);
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, buffer, data_len, ESP_LOG_INFO);

    if (ble_uuid_cmp(ctxt->chr->uuid, &NOTIFICATION_CHAR_UUID.u) == 0) {
        handle_notification_write(buffer, data_len);
        return 0;
    }

    if (ble_uuid_cmp(ctxt->chr->uuid, &CONTROL_CHAR_UUID.u) == 0) {
        handle_control_write(buffer, data_len);
        return 0;
    }

    ESP_LOGW(TAG, "Write to unknown characteristic (attr=%u) ignored", attr_handle);
    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def gatt_svr_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &SMARTWATCH_SERVICE_UUID.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid = &NOTIFICATION_CHAR_UUID.u,
                .access_cb = gatt_svr_chr_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
                .val_handle = &s_notification_handle,
            },
            {
                .uuid = &CONTROL_CHAR_UUID.u,
                .access_cb = gatt_svr_chr_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
                .val_handle = &s_control_handle,
            },
            {0},
        },
    },
    {0},
};

static int ble_gap_event(struct ble_gap_event *event, void *arg) {
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                ESP_LOGI(TAG, "BLE connected");
                s_ble_connected = true;
                s_ble_advertising = false;
            } else {
                ESP_LOGW(TAG, "BLE connect failed; status=%d", event->connect.status);
                ble_app_advertise();
            }
            break;
        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGI(TAG, "BLE disconnected; reason=%d", event->disconnect.reason);
            s_ble_connected = false;
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
        default:
            break;
    }
    return 0;
}

static int ble_on_gatt_register(struct ble_gatt_register_ctxt *ctxt, void *arg) {
    char uuid_str[BLE_UUID_STR_LEN];

    switch (ctxt->op) {
        case BLE_GATT_REGISTER_OP_SVC:
            ESP_LOGI(TAG, "Registered service %s with handle=%u",
                     ble_uuid_to_str(ctxt->svc.svc_def->uuid, uuid_str), ctxt->svc.handle);
            break;
        case BLE_GATT_REGISTER_OP_CHR:
            ESP_LOGI(TAG, "Registered characteristic %s with def_handle=%u val_handle=%u",
                     ble_uuid_to_str(ctxt->chr.chr_def->uuid, uuid_str), ctxt->chr.def_handle, ctxt->chr.val_handle);
            break;
        case BLE_GATT_REGISTER_OP_DSC:
            ESP_LOGI(TAG, "Registered descriptor %s with handle=%u",
                     ble_uuid_to_str(ctxt->dsc.dsc_def->uuid, uuid_str), ctxt->dsc.handle);
            break;
        default:
            ESP_LOGW(TAG, "Unknown GATT registration op=%d", ctxt->op);
            break;
    }

    return 0;
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
        ESP_LOGI(TAG, "Advertising started (addr_type=%u)", s_addr_type);
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
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t ble_manager_init(ble_notification_callback_t notification_cb,
                           ble_control_callback_t control_cb) {
    s_notification_cb = notification_cb;
    s_control_cb = control_cb;

    s_notif_mutex = xSemaphoreCreateMutex();

    esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);

    // esp_nimble_hci_and_controller_init() is removed in IDF v5.0+
    // nimble_port_init() handles the controller init if configured.

    esp_err_t ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init NimBLE host: %s", esp_err_to_name(ret));
        return ret;
    }

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
