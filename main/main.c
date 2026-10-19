/**
 * @file main.c
 * @brief Nibble Keypad - ESP32-C3 4-Key BLE HID Keyboard Firmware
 *
 * Implements a Bluetooth Low Energy (BLE) Human Interface Device (HID) keyboard
 * using Apache NimBLE and ESP-IDF. Includes atomic 4-key debouncing, standard 8-byte
 * HID reports, "Just Works" pairing, a 2-stage persistent power manager, and
 * configurable/persistent key mappings with full modifier combo support (Ctrl, Shift, Alt, GUI).
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
#include "services/bas/ble_svc_bas.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "host/ble_gatt.h"
#include "host/ble_store.h"
#include "host/ble_hs_mbuf.h"

static const char *TAG = "keyboard";

// ============================================================================
// Hardware & Pin Configuration
// ============================================================================
#define NUM_KEYS 4
#define DEBOUNCE_READS 5
#define SCAN_INTERVAL_MS 10

/**
 * @note ESP32-C3 Hardware Notice:
 * Keypad Physical Layout (2x2 Grid):
 *   [ Key 1: GPIO 8  ]  [ Key 2: GPIO 10 ]
 *   [ Key 3: GPIO 20 ]  [ Key 4: GPIO 9  ]
 *
 * GPIO 9 (Key 4) is the chip's BOOT strapping pin. If Key 4 is held down while powering on
 * or resetting, the ESP32-C3 enters UART bootloader / download mode instead of booting SPI flash.
 * GPIO 8 (Key 1) also controls ROM boot log output.
 */
static const gpio_num_t key_pins[NUM_KEYS] = { GPIO_NUM_8, GPIO_NUM_10, GPIO_NUM_20, GPIO_NUM_9 };
static const char *key_names[NUM_KEYS] = { "Key1", "Key2", "Key3", "Key4" };

// ============================================================================
// Configurable Key Mappings & Combos (Stored Persistently in NVS)
// ============================================================================

/**
 * @brief Standard USB HID Keyboard Modifier Bitmasks.
 */
#define KEY_MOD_NONE       0x00
#define KEY_MOD_LCTRL      (1 << 0) // 0x01: Left Control
#define KEY_MOD_LSHIFT     (1 << 1) // 0x02: Left Shift
#define KEY_MOD_LALT       (1 << 2) // 0x04: Left Alt
#define KEY_MOD_LMETA      (1 << 3) // 0x08: Left GUI / Windows / Command
#define KEY_MOD_RCTRL      (1 << 4) // 0x10: Right Control
#define KEY_MOD_RSHIFT     (1 << 5) // 0x20: Right Shift
#define KEY_MOD_RALT       (1 << 6) // 0x40: Right Alt (AltGr)
#define KEY_MOD_RMETA      (1 << 7) // 0x80: Right GUI

/**
 * @brief Key mapping structure supporting arbitrary modifier combos + keycodes.
 *
 * Examples:
 * - Simple 'h':             { .modifiers = KEY_MOD_NONE, .keycode = 0x0B }
 * - Combo 'Ctrl+Shift+v':   { .modifiers = KEY_MOD_LCTRL | KEY_MOD_LSHIFT, .keycode = 0x19 }
 * - Copy 'Ctrl+c':          { .modifiers = KEY_MOD_LCTRL, .keycode = 0x06 }
 * - Paste 'Ctrl+v':         { .modifiers = KEY_MOD_LCTRL, .keycode = 0x19 }
 * - Pure modifier (Shift):  { .modifiers = KEY_MOD_LSHIFT, .keycode = 0x00 }
 */
typedef struct {
    uint8_t modifiers; /**< Bitmask of modifier keys (KEY_MOD_*) */
    uint8_t keycode;   /**< Primary HID usage code (0x04='a', 0x19='v', etc.) */
} key_mapping_t;

#define NVS_NAMESPACE_CFG "kb_cfg"
#define NVS_KEY_KEY_MAP   "key_map"

// Default key assignments: Key 1='a', Key 2='b', Key 3='c', Key 4='d'
static const key_mapping_t s_default_key_mappings[NUM_KEYS] = {
    { .modifiers = KEY_MOD_NONE, .keycode = 0x04 }, // Key 1: 'a'
    { .modifiers = KEY_MOD_NONE, .keycode = 0x05 }, // Key 2: 'b'
    { .modifiers = KEY_MOD_NONE, .keycode = 0x06 }, // Key 3: 'c'
    { .modifiers = KEY_MOD_NONE, .keycode = 0x07 }, // Key 4: 'd'
};

static key_mapping_t s_key_mappings[NUM_KEYS];

/**
 * @brief Get the configuration of a specific key.
 *
 * @param index Key index (0 to NUM_KEYS - 1).
 * @param[out] out_mapping Pointer to receive key mapping.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG on invalid index or NULL pointer.
 */
esp_err_t keyboard_get_key_mapping(size_t index, key_mapping_t *out_mapping)
{
    if (index >= NUM_KEYS || out_mapping == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_mapping = s_key_mappings[index];
    return ESP_OK;
}

/**
 * @brief Configure and persist in NVS flash a key combo for a specific key.
 *
 * @param index Key index (0 to NUM_KEYS - 1).
 * @param modifiers Modifier bitmask (e.g. KEY_MOD_LCTRL | KEY_MOD_LSHIFT).
 * @param keycode Primary HID usage code (e.g. 0x19 for 'v').
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG on invalid index.
 */
esp_err_t keyboard_set_key_mapping(size_t index, uint8_t modifiers, uint8_t keycode)
{
    if (index >= NUM_KEYS) {
        return ESP_ERR_INVALID_ARG;
    }
    s_key_mappings[index].modifiers = modifiers;
    s_key_mappings[index].keycode = keycode;

    nvs_handle_t nvs_h;
    esp_err_t err = nvs_open(NVS_NAMESPACE_CFG, NVS_READWRITE, &nvs_h);
    if (err == ESP_OK) {
        nvs_set_blob(nvs_h, NVS_KEY_KEY_MAP, s_key_mappings, sizeof(s_key_mappings));
        nvs_commit(nvs_h);
        nvs_close(nvs_h);
    }
    ESP_LOGI(TAG, "Key %s updated: mod=0x%02X, code=0x%02X",
             key_names[index], modifiers, keycode);
    return ESP_OK;
}

/**
 * @brief Bulk update and persist all key mappings in NVS flash.
 *
 * @param mappings Array of NUM_KEYS key mappings.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if mappings pointer is NULL.
 */
esp_err_t keyboard_set_all_key_mappings(const key_mapping_t mappings[NUM_KEYS])
{
    if (mappings == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(s_key_mappings, mappings, sizeof(s_key_mappings));

    nvs_handle_t nvs_h;
    esp_err_t err = nvs_open(NVS_NAMESPACE_CFG, NVS_READWRITE, &nvs_h);
    if (err == ESP_OK) {
        nvs_set_blob(nvs_h, NVS_KEY_KEY_MAP, s_key_mappings, sizeof(s_key_mappings));
        nvs_commit(nvs_h);
        nvs_close(nvs_h);
    }
    ESP_LOGI(TAG, "All 4 key mappings bulk updated via configuration");
    return ESP_OK;
}

/**
 * @brief Load persistent key mappings from NVS flash, or save defaults on first boot.
 */
static void load_key_mappings(void)
{
    memcpy(s_key_mappings, s_default_key_mappings, sizeof(s_key_mappings));

    nvs_handle_t nvs_h;
    esp_err_t err = nvs_open(NVS_NAMESPACE_CFG, NVS_READONLY, &nvs_h);
    if (err == ESP_OK) {
        size_t required_size = sizeof(s_key_mappings);
        err = nvs_get_blob(nvs_h, NVS_KEY_KEY_MAP, s_key_mappings, &required_size);
        if (err != ESP_OK || required_size != sizeof(s_key_mappings)) {
            memcpy(s_key_mappings, s_default_key_mappings, sizeof(s_key_mappings));
        }
        nvs_close(nvs_h);
    } else {
        // First boot: save initial defaults to NVS
        if (nvs_open(NVS_NAMESPACE_CFG, NVS_READWRITE, &nvs_h) == ESP_OK) {
            nvs_set_blob(nvs_h, NVS_KEY_KEY_MAP, s_key_mappings, sizeof(s_key_mappings));
            nvs_commit(nvs_h);
            nvs_close(nvs_h);
            ESP_LOGI(TAG, "Initialized default key mappings in NVS");
        }
    }

    for (int i = 0; i < NUM_KEYS; ++i) {
        ESP_LOGI(TAG, "Key %s mapping: mod=0x%02X, code=0x%02X",
                 key_names[i], s_key_mappings[i].modifiers, s_key_mappings[i].keycode);
    }
}

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
static const char *DEVICE_NAME = "Nibble Keypad";

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

#define NVS_KEY_PWR_MODE "pwr_mode"

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
// Factory Reset & Device Unpairing Logic
// ============================================================================

/**
 * @brief Asynchronous FreeRTOS task to execute factory reset.
 *
 * 1. Delays 300 ms so that any in-flight GATT write response is delivered to the host.
 * 2. Restores in-memory key mappings ('a', 'b', 'c', 'd') and power profile (NORMAL).
 * 3. Erases the "kb_cfg" NVS namespace (persistent keys & power profile).
 * 4. Clears all bonded peer encryption keys & CCCDs from NimBLE store.
 * 5. Cleanly terminates the active BLE connection (if connected).
 * 6. Delays 200 ms to transmit the disconnect packet, then reboots the ESP32-C3 via esp_restart().
 */
static void factory_reset_task(void *arg)
{
    ESP_LOGW(TAG, "Factory reset sequence initiated: waiting 300ms for GATT response transmission...");
    vTaskDelay(pdMS_TO_TICKS(300));

    // 1. Restore defaults in memory
    memcpy(s_key_mappings, s_default_key_mappings, sizeof(s_key_mappings));
    s_power_mode = POWER_PROFILE_NORMAL;

    // 2. Erase keyboard configuration from NVS
    nvs_handle_t nvs_h;
    if (nvs_open(NVS_NAMESPACE_CFG, NVS_READWRITE, &nvs_h) == ESP_OK) {
        nvs_erase_all(nvs_h);
        nvs_commit(nvs_h);
        nvs_close(nvs_h);
        ESP_LOGI(TAG, "Erased NVS namespace '%s'", NVS_NAMESPACE_CFG);
    }

    // 3. Clear all bonded security records & pairing keys from NimBLE (forgets paired device)
    int rc = ble_store_clear();
    ESP_LOGI(TAG, "NimBLE security store cleared (rc=%d)", rc);

    // 4. Terminate BLE connection if active
    if (s_ble_connected && s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ESP_LOGI(TAG, "Disconnecting peer on conn_handle=%u", s_conn_handle);
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }

    // 5. Brief delay to allow BLE disconnection packet to be transmitted, then reboot
    vTaskDelay(pdMS_TO_TICKS(200));
    ESP_LOGW(TAG, "Restarting ESP32-C3 in clean factory state...");
    esp_restart();
}

/**
 * @brief Trigger a full factory reset asynchronously.
 *
 * @return ESP_OK on success, ESP_FAIL if task creation failed.
 */
esp_err_t trigger_factory_reset(void)
{
    BaseType_t ret = xTaskCreate(factory_reset_task, "fact_rst_task", 2560, NULL, 5, NULL);
    return (ret == pdPASS) ? ESP_OK : ESP_FAIL;
}

// ============================================================================
// Custom GATT Configuration Service (UUID 0xFFF0)
// ============================================================================
#define CFG_SVC_UUID               0xFFF0
#define CFG_CHR_PWR_MODE_UUID      0xFFF1
#define CFG_CHR_KEY_MAP_UUID       0xFFF2
#define CFG_CHR_FACTORY_RESET_UUID 0xFFF3

static const ble_uuid16_t s_cfg_svc_uuid = BLE_UUID16_INIT(CFG_SVC_UUID);
static const ble_uuid16_t s_cfg_chr_pwr_uuid = BLE_UUID16_INIT(CFG_CHR_PWR_MODE_UUID);
static const ble_uuid16_t s_cfg_chr_key_map_uuid = BLE_UUID16_INIT(CFG_CHR_KEY_MAP_UUID);
static const ble_uuid16_t s_cfg_chr_reset_uuid = BLE_UUID16_INIT(CFG_CHR_FACTORY_RESET_UUID);

static uint16_t s_cfg_chr_pwr_handle;
static uint16_t s_cfg_chr_key_map_handle;
static uint16_t s_cfg_chr_reset_handle;

static const ble_uuid16_t s_user_desc_uuid = BLE_UUID16_INIT(0x2901);
static const char s_desc_pwr[] = "Power Profile (0=Normal, 1=EnergySave)";
static const char s_desc_keys[] = "Key Mappings (4x 2-byte [mod, keycode])";
static const char s_desc_reset[] = "Factory Reset (Write 1 to clear NVS and forget device)";

/**
 * @brief GATT access callback for the custom configuration service.
 */
static int cfg_gatt_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                             struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    // Handle descriptor read (Characteristic User Description 0x2901)
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_DSC) {
        if (ctxt->dsc != NULL && ctxt->dsc->arg != NULL) {
            const char *desc = (const char *)ctxt->dsc->arg;
            int rc = os_mbuf_append(ctxt->om, desc, strlen(desc));
            return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        return BLE_ATT_ERR_UNLIKELY;
    }

    const ble_uuid_t *uuid = ctxt->chr->uuid;

    // --- Characteristic 1: Power Profile (0xFFF1) ---
    if (ble_uuid_cmp(uuid, &s_cfg_chr_pwr_uuid.u) == 0) {
        if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
            uint8_t mode = (uint8_t)s_power_mode;
            int rc = os_mbuf_append(ctxt->om, &mode, sizeof(mode));
            return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        } else if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
            uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
            if (len != 1) {
                return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            }
            uint8_t new_mode = 0;
            int rc = ble_hs_mbuf_to_flat(ctxt->om, &new_mode, sizeof(new_mode), NULL);
            if (rc != 0) {
                return BLE_ATT_ERR_UNLIKELY;
            }
            if (new_mode > POWER_PROFILE_ENERGY_SAVE) {
                return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            }
            power_manager_set_profile((power_profile_mode_t)new_mode);
            return 0;
        }
    }

    // --- Characteristic 2: Key Mapping Table (0xFFF2) ---
    if (ble_uuid_cmp(uuid, &s_cfg_chr_key_map_uuid.u) == 0) {
        if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
            int rc = os_mbuf_append(ctxt->om, s_key_mappings, sizeof(s_key_mappings));
            return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        } else if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
            uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
            if (len != sizeof(s_key_mappings)) {
                return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            }
            key_mapping_t new_map[NUM_KEYS];
            int rc = ble_hs_mbuf_to_flat(ctxt->om, new_map, sizeof(new_map), NULL);
            if (rc != 0) {
                return BLE_ATT_ERR_UNLIKELY;
            }
            keyboard_set_all_key_mappings(new_map);
            return 0;
        }
    }

    // --- Characteristic 3: Factory Reset / Forget Device (0xFFF3) ---
    if (ble_uuid_cmp(uuid, &s_cfg_chr_reset_uuid.u) == 0) {
        if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
            uint8_t zero = 0;
            int rc = os_mbuf_append(ctxt->om, &zero, sizeof(zero));
            return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        } else if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
            uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
            if (len < 1) {
                return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            }
            uint8_t cmd = 0;
            int rc = ble_hs_mbuf_to_flat(ctxt->om, &cmd, 1, NULL);
            if (rc != 0) {
                return BLE_ATT_ERR_UNLIKELY;
            }
            if (cmd != 0) {
                ESP_LOGW(TAG, "GATT write: Factory reset triggered (cmd=0x%02X)", cmd);
                trigger_factory_reset();
            }
            return 0;
        }
    }

    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def s_cfg_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_cfg_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[])
        {
            {
                // Characteristic 1: Power Profile (0xFFF1)
                .uuid = &s_cfg_chr_pwr_uuid.u,
                .access_cb = cfg_gatt_access_cb,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
                .val_handle = &s_cfg_chr_pwr_handle,
                .descriptors = (struct ble_gatt_dsc_def[])
                {
                    {
                        .uuid = &s_user_desc_uuid.u,
                        .att_flags = BLE_ATT_F_READ,
                        .access_cb = cfg_gatt_access_cb,
                        .arg = (void *)s_desc_pwr,
                    },
                    { 0 }
                },
            },
            {
                // Characteristic 2: Key Mapping Table (0xFFF2)
                .uuid = &s_cfg_chr_key_map_uuid.u,
                .access_cb = cfg_gatt_access_cb,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
                .val_handle = &s_cfg_chr_key_map_handle,
                .descriptors = (struct ble_gatt_dsc_def[])
                {
                    {
                        .uuid = &s_user_desc_uuid.u,
                        .att_flags = BLE_ATT_F_READ,
                        .access_cb = cfg_gatt_access_cb,
                        .arg = (void *)s_desc_keys,
                    },
                    { 0 }
                },
            },
            {
                // Characteristic 3: Factory Reset / Forget Device (0xFFF3)
                .uuid = &s_cfg_chr_reset_uuid.u,
                .access_cb = cfg_gatt_access_cb,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
                .val_handle = &s_cfg_chr_reset_handle,
                .descriptors = (struct ble_gatt_dsc_def[])
                {
                    {
                        .uuid = &s_user_desc_uuid.u,
                        .att_flags = BLE_ATT_F_READ,
                        .access_cb = cfg_gatt_access_cb,
                        .arg = (void *)s_desc_reset,
                    },
                    { 0 }
                },
            },
            { 0 } // End of characteristics
        },
    },
    { 0 } // End of services
};

/**
 * @brief Register the custom configuration GATT service with NimBLE.
 */
static void init_custom_cfg_service(void)
{
    int rc = ble_gatts_count_cfg(s_cfg_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_count_cfg failed: %d", rc);
        return;
    }
    rc = ble_gatts_add_svcs(s_cfg_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_add_svcs failed: %d", rc);
        return;
    }
    ESP_LOGI(TAG, "Custom Configuration GATT Service (0x%04X) registered successfully", CFG_SVC_UUID);
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
    .device_name = "Nibble Keypad",
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

    // Include custom configuration service UUID in scan response for easy discovery
    static ble_uuid16_t cfg_service_uuid = BLE_UUID16_INIT(CFG_SVC_UUID);
    struct ble_hs_adv_fields rsp_fields;
    memset(&rsp_fields, 0, sizeof(rsp_fields));
    rsp_fields.uuids16 = &cfg_service_uuid;
    rsp_fields.num_uuids16 = 1;
    rsp_fields.uuids16_is_complete = 1;
    ble_gap_adv_rsp_set_fields(&rsp_fields);

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
        esp_hidd_dev_battery_set(s_hid_dev, 100);
        ESP_LOGI(TAG, "HID host connected (battery reported as 100%%)");
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
 * Populates Byte 0 with the OR-ed modifier mask and Bytes 2..7 with active keycodes.
 * Supports simultaneous multi-key presses and full combos (e.g. Ctrl + Shift + V).
 *
 * @param state Array of debounced binary levels for Keys 1..4 (0 = pressed, 1 = released).
 */
static void update_keyboard_report_from_state(const int *state)
{
    memset(s_key_report, 0, sizeof(s_key_report));
    uint8_t modifiers = 0;
    int idx = 2; // Keycode array starts at byte index 2

    for (int i = 0; i < NUM_KEYS; ++i) {
        if (state[i] == 0) { // Active low: 0 indicates pressed
            modifiers |= s_key_mappings[i].modifiers;
            if (s_key_mappings[i].keycode != 0 && idx < HID_KEYBOARD_REPORT_LEN) {
                s_key_report[idx++] = s_key_mappings[i].keycode;
            }
        }
    }
    s_key_report[0] = modifiers;
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
                    ESP_LOGI(TAG, "Key %s %s (GPIO %d, mod=0x%02X, code=0x%02X)",
                             key_names[i], level == 0 ? "pressed" : "released", key_pins[i],
                             s_key_mappings[i].modifiers, s_key_mappings[i].keycode);
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

    // Load persistent key mappings and power management profile from NVS
    load_key_mappings();
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

    // Initialize battery level to 100% in GATT database (no physical battery monitor)
    ble_svc_bas_battery_level_set(100);

    // Register custom configuration GATT service (UUID 0xFFF0)
    init_custom_cfg_service();

    // Start the NimBLE host task
    host_task_start();

    ESP_LOGI(TAG, "BLE HID + Config Service initialized, waiting for sync");
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

    // Hardware failsafe: hold Key 1 (GPIO 8) + Key 3 (GPIO 20) during boot for 2s to factory reset
    if (gpio_get_level(key_pins[0]) == 0 && gpio_get_level(key_pins[2]) == 0) {
        ESP_LOGW(TAG, "Key 1 + Key 3 held at boot -> checking for factory reset combo (hold 2s)...");
        vTaskDelay(pdMS_TO_TICKS(2000));
        if (gpio_get_level(key_pins[0]) == 0 && gpio_get_level(key_pins[2]) == 0) {
            ESP_LOGW(TAG, "Hardware factory reset triggered via Key 1 + Key 3 hold!");
            nvs_flash_init();
            nvs_handle_t nvs_h;
            if (nvs_open(NVS_NAMESPACE_CFG, NVS_READWRITE, &nvs_h) == ESP_OK) {
                nvs_erase_all(nvs_h);
                nvs_commit(nvs_h);
                nvs_close(nvs_h);
            }
            ble_store_clear();
            esp_restart();
        }
    }

    init_ble_hid();

    xTaskCreate(key_task, "key_task", 3072, NULL, 5, NULL);

    ESP_LOGI(TAG, "Key scanner + BLE HID started");
}
