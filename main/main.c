/**
 * @file main.c
 * @brief Nimble 4 - ESP32-C3 4-Key BLE HID Keyboard Firmware
 *
 * Implements a Bluetooth Low Energy (BLE) Human Interface Device (HID) keyboard
 * using Apache NimBLE and ESP-IDF. Includes atomic 4-key debouncing, standard 8-byte
 * HID reports, "Just Works" pairing, and a 2-stage persistent power manager.
 */

#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_bt.h"
#include "esp_err.h"
#include "esp_hid_common.h"
#include "esp_hidd.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_sm.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "esp_sleep.h"
#include "esp_timer.h"

static const char *TAG = "keyboard";

// ============================================================================
// Hardware & Key Configuration
// ============================================================================
#define NUM_KEYS 4
#define DEBOUNCE_READS 5
#define SCAN_INTERVAL_MS 10

/**
 * @note ESP32-C3 Hardware Notice:
 * GPIO 9 is the chip's BOOT strapping pin. If Key 3 is held down while powering on
 * or resetting, the ESP32-C3 enters UART bootloader / download mode instead of booting SPI flash.
 * GPIO 8 also controls ROM boot log output.
 * If redesigning hardware, prefer non-strapping pins (e.g. GPIO 0..7, 21).
 */
static const gpio_num_t key_pins[NUM_KEYS] = { GPIO_NUM_10, GPIO_NUM_8, GPIO_NUM_9, GPIO_NUM_20 };
static const char *key_names[NUM_KEYS] = { "Key1", "Key2", "Key3", "Key4" };
static const uint8_t key_hid_usages[NUM_KEYS] = { 0x04, 0x05, 0x06, 0x07 }; // 'a', 'b', 'c', 'd'

// ============================================================================
// HID Report Configuration
// ============================================================================
#define HID_KEYBOARD_REPORT_ID 1
#define HID_KEYBOARD_REPORT_LEN 8

static esp_hidd_dev_t *s_hid_dev = NULL;
static volatile bool s_ble_connected = false;
static volatile bool s_report_pending = false;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint8_t s_key_report[HID_KEYBOARD_REPORT_LEN];

// BLE Device Identity
static const char *DEVICE_NAME = "Nimble 4";

// Forward declaration from NimBLE store config
void ble_store_config_init(void);

// ============================================================================
// Power Management Profiles (Configurable & Persistent in NVS)
// ============================================================================

/**
 * @brief Available power profiles controlling idle and sleep timeout thresholds.
 */
typedef enum {
    POWER_PROFILE_NORMAL = 0,       /**< Normal mode: 30s connected idle / 10 min sleep */
    POWER_PROFILE_ENERGY_SAVE = 1,   /**< Energy save mode: 10s connected idle / 5 min sleep */
} power_profile_mode_t;

/**
 * @brief Timing parameters for a power profile.
 */
typedef struct {
    uint32_t idle_timeout_sec;   /**< Stage 1: Time until connected idle (slave latency enabled) */
    uint32_t sleep_timeout_sec;  /**< Stage 2: Time until radio disconnect and light sleep */
    const char *name;            /**< Human-readable profile name */
} power_profile_config_t;

static const power_profile_config_t g_power_profiles[] = {
    [POWER_PROFILE_NORMAL]      = { .idle_timeout_sec = 30, .sleep_timeout_sec = 600, .name = "NORMAL (30s idle / 10m sleep)" },
    [POWER_PROFILE_ENERGY_SAVE] = { .idle_timeout_sec = 10, .sleep_timeout_sec = 300, .name = "ENERGY_SAVE (10s idle / 5m sleep)" },
};

#define NVS_NAMESPACE_CFG "kb_cfg"
#define NVS_KEY_PWR_MODE  "pwr_mode"

static power_profile_mode_t s_power_mode = POWER_PROFILE_ENERGY_SAVE;
static bool s_in_idle_conn_params = false;

/**
 * @brief Retrieve the currently active power management profile.
 *
 * @return power_profile_mode_t Active profile enum (0 = NORMAL, 1 = ENERGY_SAVE).
 */
power_profile_mode_t power_manager_get_profile(void)
{
    return s_power_mode;
}

/**
 * @brief Set the active power management profile and commit it to NVS flash.
 *
 * @param mode Desired power profile.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if mode is out of range.
 */
esp_err_t power_manager_set_profile(power_profile_mode_t mode)
{
    if (mode > POWER_PROFILE_ENERGY_SAVE) {
        return ESP_ERR_INVALID_ARG;
    }
    s_power_mode = mode;

    nvs_handle_t nvs_h;
    esp_err_t err = nvs_open(NVS_NAMESPACE_CFG, NVS_READWRITE, &nvs_h);
    if (err == ESP_OK) {
        nvs_set_u8(nvs_h, NVS_KEY_PWR_MODE, (uint8_t)mode);
        nvs_commit(nvs_h);
        nvs_close(nvs_h);
    }
    ESP_LOGI(TAG, "Power profile updated to: %s", g_power_profiles[mode].name);
    return ESP_OK;
}

/**
 * @brief Load persistent power management profile from NVS storage.
 */
static void load_power_profile(void)
{
    nvs_handle_t nvs_h;
    esp_err_t err = nvs_open(NVS_NAMESPACE_CFG, NVS_READONLY, &nvs_h);
    if (err == ESP_OK) {
        uint8_t mode = 0;
        if (nvs_get_u8(nvs_h, NVS_KEY_PWR_MODE, &mode) == ESP_OK) {
            if (mode <= POWER_PROFILE_ENERGY_SAVE) {
                s_power_mode = (power_profile_mode_t)mode;
            }
        }
        nvs_close(nvs_h);
    }
    ESP_LOGI(TAG, "Active power profile: %s", g_power_profiles[s_power_mode].name);
}

/**
 * @brief Dynamically request BLE connection parameter updates for Stage 1 low-power idle.
 *
 * @param idle If true, enables slave latency (30 connection intervals).
 *             If false, restores low-latency parameters (latency = 0) for active typing.
 */
static void update_connection_power_mode(bool idle)
{
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE || !s_ble_connected) {
        return;
    }
    if (s_in_idle_conn_params == idle) {
        return;
    }
    s_in_idle_conn_params = idle;

    struct ble_gap_upd_params params;
    if (idle) {
        // Stage 1 (Connected Idle): Interval 30-50ms, slave latency=30 (skip up to 30 events = ~1s low power sleep)
        params.itvl_min = BLE_GAP_ADV_ITVL_MS(30);
        params.itvl_max = BLE_GAP_ADV_ITVL_MS(50);
        params.latency = 30;
        params.supervision_timeout = 500; // 5000 ms
        ESP_LOGI(TAG, "BLE power mode: Connected Idle (slave latency = 30)");
    } else {
        // Stage 0 (Active): Interval 10-15ms, latency=0 for instant response
        params.itvl_min = 8;  // 10ms
        params.itvl_max = 12; // 15ms
        params.latency = 0;
        params.supervision_timeout = 500;
        ESP_LOGI(TAG, "BLE power mode: Active (latency = 0)");
    }
    ble_gap_update_params(s_conn_handle, &params);
}

// ============================================================================
// HID Report Map & Configuration
// ============================================================================

/**
 * @brief Standard USB HID Keyboard Report Descriptor.
 *
 * Layout:
 * - 1 byte Modifier keys (Ctrl, Shift, Alt, GUI)
 * - 1 byte Reserved / OEM padding
 * - 5 bits LED output indicators (NumLock, CapsLock, ScrollLock, Compose, Kana) + 3 bits padding
 * - 6 bytes Key array (6-key rollover)
 */
static const uint8_t keyboard_report_map[] = {
    0x05, 0x01,                    // USAGE_PAGE (Generic Desktop)
    0x09, 0x06,                    // USAGE (Keyboard)
    0xA1, 0x01,                    // COLLECTION (Application)
    0x85, HID_KEYBOARD_REPORT_ID,  //   REPORT_ID (1)
    0x05, 0x07,                    //   USAGE_PAGE (Keyboard)
    0x19, 0xE0,                    //   USAGE_MINIMUM (Keyboard LeftControl)
    0x29, 0xE7,                    //   USAGE_MAXIMUM (Keyboard Right GUI)
    0x15, 0x00,                    //   LOGICAL_MINIMUM (0)
    0x25, 0x01,                    //   LOGICAL_MAXIMUM (1)
    0x75, 0x01,                    //   REPORT_SIZE (1)
    0x95, 0x08,                    //   REPORT_COUNT (8)
    0x81, 0x02,                    //   INPUT (Data,Var,Abs) -- Modifiers byte
    0x95, 0x01,                    //   REPORT_COUNT (1)
    0x75, 0x08,                    //   REPORT_SIZE (8)
    0x81, 0x03,                    //   INPUT (Cnst,Var,Abs) -- Reserved byte
    0x95, 0x05,                    //   REPORT_COUNT (5)
    0x75, 0x01,                    //   REPORT_SIZE (1)
    0x05, 0x08,                    //   USAGE_PAGE (LEDs)
    0x19, 0x01,                    //   USAGE_MINIMUM (Num Lock)
    0x29, 0x05,                    //   USAGE_MAXIMUM (Kana)
    0x91, 0x02,                    //   OUTPUT (Data,Var,Abs) -- LED report
    0x95, 0x01,                    //   REPORT_COUNT (1)
    0x75, 0x03,                    //   REPORT_SIZE (3)
    0x91, 0x03,                    //   OUTPUT (Cnst,Var,Abs) -- LED padding
    0x95, 0x06,                    //   REPORT_COUNT (6)
    0x75, 0x08,                    //   REPORT_SIZE (8)
    0x15, 0x00,                    //   LOGICAL_MINIMUM (0)
    0x25, 0x65,                    //   LOGICAL_MAXIMUM (101)
    0x05, 0x07,                    //   USAGE_PAGE (Keyboard)
    0x19, 0x00,                    //   USAGE_MINIMUM (Reserved)
    0x29, 0x65,                    //   USAGE_MAXIMUM (Keyboard Application)
    0x81, 0x00,                    //   INPUT (Data,Ary,Abs) -- 6-key rollover array
    0xC0                           // END_COLLECTION
};

static esp_hid_raw_report_map_t report_maps[] = {
    { .data = keyboard_report_map, .len = sizeof(keyboard_report_map) }
};

static esp_hid_device_config_t hid_config = {
    .vendor_id = 0x16C0,
    .product_id = 0x05DF,
    .version = 0x0100,
    .device_name = "Nimble 4",
    .manufacturer_name = "Keyboard",
    .serial_number = "0001",
    .report_maps = report_maps,
    .report_maps_len = 1,
};

static int ble_gap_event(struct ble_gap_event *event, void *arg);

/**
 * @brief Configure advertising packet fields and start general discoverable BLE advertising.
 */
static void ble_start_advertising(void)
{
    if (ble_gap_adv_active()) {
        ESP_LOGI(TAG, "Advertising is already active");
        return;
    }

    static ble_uuid16_t hid_service_uuid = BLE_UUID16_INIT(0x1812);
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));

    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.appearance = ESP_HID_APPEARANCE_KEYBOARD;
    fields.appearance_is_present = 1;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    fields.name = (uint8_t *)DEVICE_NAME;
    fields.name_len = strlen(DEVICE_NAME);
    fields.name_is_complete = 1;
    fields.uuids16 = &hid_service_uuid;
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_set_fields failed: %d", rc);
        return;
    }

    uint8_t own_addr_type;
    rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        own_addr_type = BLE_OWN_ADDR_PUBLIC;
    }

    struct ble_gap_adv_params adv_params;
    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    adv_params.itvl_min = BLE_GAP_ADV_ITVL_MS(30);
    adv_params.itvl_max = BLE_GAP_ADV_ITVL_MS(50);

    rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &adv_params, ble_gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "ble_gap_adv_start failed: %d", rc);
    } else {
        ESP_LOGI(TAG, "BLE advertising started successfully as '%s'", DEVICE_NAME);
    }
}

/**
 * @brief NimBLE Host task loop entry point.
 */
static void ble_host_task(void *param)
{
    ESP_LOGI(TAG, "BLE host task started");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/**
 * @brief NimBLE GAP event handler for connection, disconnection, and security requests.
 */
static int ble_gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        ESP_LOGI(TAG, "BLE connection %s; status=%d, handle=%u",
                 event->connect.status == 0 ? "established" : "failed",
                 event->connect.status, event->connect.conn_handle);
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            s_in_idle_conn_params = false;
        } else {
            s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            ble_start_advertising();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "BLE disconnected; reason=%d", event->disconnect.reason);
        s_ble_connected = false;
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_in_idle_conn_params = false;
        ble_start_advertising();
        return 0;

    case BLE_GAP_EVENT_PASSKEY_ACTION: {
        struct ble_sm_io io = {0};
        io.action = event->passkey.params.action;
        if (event->passkey.params.action == BLE_SM_IOACT_NUMCMP) {
            io.numcmp_accept = 1;
        }
        ble_sm_inject_io(event->passkey.conn_handle, &io);
        return 0;
    }

    default:
        return 0;
    }
}

/**
 * @brief ESP HID Device callback invoked when HID events occur.
 */
static void hidd_event_callback(void *handler_args, esp_event_base_t base, int32_t id, void *event_data)
{
    esp_hidd_event_t event = (esp_hidd_event_t)id;
    esp_hidd_event_data_t *param = (esp_hidd_event_data_t *)event_data;

    switch (event) {
    case ESP_HIDD_START_EVENT:
        ESP_LOGI(TAG, "HID device started -> beginning advertising");
        ble_start_advertising();
        break;
    case ESP_HIDD_CONNECT_EVENT:
        s_ble_connected = true;
        s_report_pending = true;
        s_in_idle_conn_params = false;
        ESP_LOGI(TAG, "HID host connected");
        break;
    case ESP_HIDD_DISCONNECT_EVENT:
        s_ble_connected = false;
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_in_idle_conn_params = false;
        ESP_LOGI(TAG, "HID host disconnected; reason=%d", param->disconnect.reason);
        ble_start_advertising();
        break;
    default:
        break;
    }
}

/**
 * @brief Transmit the current 8-byte key report to the connected host via GATT notification.
 */
static esp_err_t send_keyboard_report(void)
{
    if (s_hid_dev == NULL || !s_ble_connected) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_hidd_dev_input_set(s_hid_dev, 0, HID_KEYBOARD_REPORT_ID, s_key_report, sizeof(s_key_report));
}

/**
 * @brief Update the internal 8-byte HID report buffer from current key debounced state.
 *
 * @param state Array of debounced binary levels for Keys 1..4 (0 = pressed, 1 = released).
 */
static void update_keyboard_report_from_state(const int *state)
{
    memset(s_key_report, 0, sizeof(s_key_report));
    int idx = 2; // Keycode array starts at byte index 2
    for (int i = 0; i < NUM_KEYS && idx < HID_KEYBOARD_REPORT_LEN; ++i) {
        if (state[i] == 0) { // Active low: 0 indicates pressed
            s_key_report[idx++] = key_hid_usages[i];
        }
    }
}

/**
 * @brief Key scanning and power manager FreeRTOS task.
 *
 * Runs every 10 ms to:
 * 1. Debounce GPIO inputs across 5 consecutive samples.
 * 2. Transmit bundled HID reports when keys change.
 * 3. Step through Stage 1 (Connected Idle) and Stage 2 (Sleep Mode) based on inactivity.
 */
void key_task(void *arg)
{
    int debounced_state[NUM_KEYS];
    int stable_count[NUM_KEYS];

    for (int i = 0; i < NUM_KEYS; ++i) {
        debounced_state[i] = gpio_get_level(key_pins[i]);
        stable_count[i] = 0;
    }

    int64_t last_activity_us = esp_timer_get_time();

    while (1) {
        bool state_changed = false;

        for (int i = 0; i < NUM_KEYS; ++i) {
            int level = gpio_get_level(key_pins[i]);
            if (level != debounced_state[i]) {
                stable_count[i]++;
                if (stable_count[i] >= DEBOUNCE_READS) {
                    debounced_state[i] = level;
                    stable_count[i] = 0;
                    state_changed = true;
                    ESP_LOGI(TAG, "Key %s %s (GPIO %d)", key_names[i],
                             level == 0 ? "pressed" : "released", key_pins[i]);
                }
            } else {
                stable_count[i] = 0;
            }
        }

        // On any key change: mark pending report, reset timer, and restore active connection latency
        if (state_changed) {
            s_report_pending = true;
            last_activity_us = esp_timer_get_time();
            if (s_in_idle_conn_params) {
                update_connection_power_mode(false);
            }
        }

        if (s_ble_connected && s_report_pending) {
            update_keyboard_report_from_state(debounced_state);
            esp_err_t err = send_keyboard_report();
            if (err == ESP_OK) {
                s_report_pending = false;
            } else {
                ESP_LOGW(TAG, "send report failed: %s (will retry)", esp_err_to_name(err));
            }
        }

        // Check if all keys are currently released
        bool all_keys_idle = true;
        for (int i = 0; i < NUM_KEYS; ++i) {
            if (debounced_state[i] == 0) {
                all_keys_idle = false;
                last_activity_us = esp_timer_get_time();
                break;
            }
        }

        if (all_keys_idle && !s_report_pending) {
            uint32_t idle_sec = g_power_profiles[s_power_mode].idle_timeout_sec;
            uint32_t sleep_sec = g_power_profiles[s_power_mode].sleep_timeout_sec;
            int64_t elapsed_sec = (esp_timer_get_time() - last_activity_us) / 1000000LL;

            // Stage 2: Sleep Mode (5 min in ENERGY_SAVE, 10 min in NORMAL)
            if (elapsed_sec >= (int64_t)sleep_sec) {
                ESP_LOGI(TAG, "Inactivity timeout (%lu s) reached -> entering sleep mode...", (unsigned long)sleep_sec);

                // Terminate connection cleanly so host does not wait for supervision timeout
                if (s_ble_connected && s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
                    ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
                    vTaskDelay(pdMS_TO_TICKS(100));
                }

                // Stop advertising to save radio power
                if (ble_gap_adv_active()) {
                    ble_gap_adv_stop();
                }

                // Configure GPIO wakeup on key pins (active-low)
                for (int i = 0; i < NUM_KEYS; ++i) {
                    gpio_wakeup_enable(key_pins[i], GPIO_INTR_LOW_LEVEL);
                }
                esp_sleep_enable_gpio_wakeup();

                vTaskDelay(pdMS_TO_TICKS(50)); // Allow UART log to complete

                esp_light_sleep_start();

                // Execution resumes here upon key press interrupt
                ESP_LOGI(TAG, "Woke up from sleep! Resuming...");
                last_activity_us = esp_timer_get_time();
                s_in_idle_conn_params = false;

                // Restart advertising so host can reconnect
                if (!s_ble_connected) {
                    ble_start_advertising();
                }
            }
            // Stage 1: Connected Idle (10s in ENERGY_SAVE, 30s in NORMAL)
            else if (elapsed_sec >= (int64_t)idle_sec) {
                if (!s_in_idle_conn_params && s_ble_connected) {
                    update_connection_power_mode(true);
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(SCAN_INTERVAL_MS));
    }
}

/**
 * @brief Initialize FreeRTOS wrapper for NimBLE host task.
 */
static void host_task_start(void)
{
    nimble_port_freertos_init(ble_host_task);
}

/**
 * @brief Initialize Bluetooth controller, NimBLE stack, and HID service.
 */
static void init_ble_hid(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Load persistent power management profile from NVS
    load_power_profile();

    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BLE));
    ESP_ERROR_CHECK(esp_nimble_init());

    // Security & bonding configuration for headless 4-key macropad ("Just Works")
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ID | BLE_SM_PAIR_KEY_DIST_ENC;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ID | BLE_SM_PAIR_KEY_DIST_ENC;

    ble_store_config_init();
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    int rc = ble_svc_gap_device_name_set(DEVICE_NAME);
    if (rc != 0) {
        ESP_LOGW(TAG, "ble_svc_gap_device_name_set rc=%d", rc);
    }

    /*
     * Initialize HID device BEFORE starting the NimBLE host task.
     * esp_hidd_dev_init registers the HID GATT services and sets ble_hs_cfg.sync_cb
     * to internal handler nimble_host_synced, which fires ESP_HIDD_START_EVENT
     * when the host task establishes sync.
     */
    ESP_ERROR_CHECK(esp_hidd_dev_init(&hid_config, ESP_HID_TRANSPORT_BLE, hidd_event_callback, &s_hid_dev));

    // Start the NimBLE host task
    host_task_start();

    ESP_LOGI(TAG, "BLE HID initialized, waiting for sync");
}

/**
 * @brief Application entry point.
 */
void app_main(void)
{
    // Configure GPIOs as inputs with internal pull-up
    gpio_config_t io_conf = {
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    for (int i = 0; i < NUM_KEYS; ++i) {
        io_conf.pin_bit_mask = (1ULL << key_pins[i]);
        ESP_ERROR_CHECK(gpio_config(&io_conf));
    }

    init_ble_hid();

    xTaskCreate(key_task, "key_task", 3072, NULL, 5, NULL);

    ESP_LOGI(TAG, "Key scanner + BLE HID started");
}
