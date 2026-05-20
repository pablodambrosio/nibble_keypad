#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "keyboard";

#define NUM_KEYS 4
#define DEBOUNCE_READS 5
#define SCAN_INTERVAL_MS 10

static const gpio_num_t key_pins[NUM_KEYS] = {GPIO_NUM_10 , GPIO_NUM_8, GPIO_NUM_9, GPIO_NUM_20 };
static const char *key_names[NUM_KEYS] = { "Key 1", "Key 2", "Key 3", "Key 4" };

void key_task(void *arg)
{
    int prev_state[NUM_KEYS];
    int stable_count[NUM_KEYS];

    // Inputs are pulled-up, so idle=1
    for (int i = 0; i < NUM_KEYS; ++i) {
        prev_state[i] = 1;
        stable_count[i] = 0;
    }

    while (1) {
        for (int i = 0; i < NUM_KEYS; ++i) {
            int level = gpio_get_level(key_pins[i]);

            if (level != prev_state[i]) {
                stable_count[i]++;
                if (stable_count[i] >= DEBOUNCE_READS) {
                    int old = prev_state[i];
                    prev_state[i] = level;
                    stable_count[i] = 0;
                    // active low: press is 1 -> 0
                    if (old == 1 && level == 0) {
                        printf("Key %s pressed (GPIO %d)\n", key_names[i], key_pins[i]);
                        ESP_LOGI(TAG, "Key %s pressed (GPIO %d)", key_names[i], key_pins[i]);
                    }
                    // release can be logged similarly if desired
                }
            } else {
                stable_count[i] = 0;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(SCAN_INTERVAL_MS));
    }
}

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

    // Create the key scanning task
    xTaskCreate(key_task, "key_task", 2048, NULL, 5, NULL);

    ESP_LOGI(TAG, "Key scanner started (D7..D10 -> GPIO %d..%d)", key_pins[0], key_pins[NUM_KEYS-1]);
}
