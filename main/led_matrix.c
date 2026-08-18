#include "led_matrix.h"

#include <stdlib.h>

#include "driver/rmt_tx.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"
#include "led_strip_rmt.h"

#include "board_config.h"
#include "rf_receiver.h"

static const char *TAG = "led_matrix";
static led_strip_handle_t s_strip;

static uint32_t matrix_index(uint16_t x, uint16_t y)
{
    /* First pixel is top-left; each column runs vertically and alternates. */
    const uint16_t physical_y = (x & 1U)
        ? (CONFIG_LED_MATRIX_HEIGHT - 1U - y)
        : y;
    return ((uint32_t) x * CONFIG_LED_MATRIX_HEIGHT) + physical_y;
}

static uint8_t scale_brightness(uint8_t value)
{
    return (uint8_t) (((uint16_t) value * CONFIG_LED_BRIGHTNESS) / 100U);
}

static void color_wheel(uint8_t position,
                        uint8_t *red, uint8_t *green, uint8_t *blue)
{
    if (position < 85U) {
        *red = 255U - position * 3U;
        *green = 0U;
        *blue = position * 3U;
    } else if (position < 170U) {
        position -= 85U;
        *red = 0U;
        *green = position * 3U;
        *blue = 255U - position * 3U;
    } else {
        position -= 170U;
        *red = position * 3U;
        *green = 255U - position * 3U;
        *blue = 0U;
    }
}

static void matrix_demo_task(void *argument)
{
    (void) argument;
    uint8_t phase = 0U;
    uint8_t rf_reaction_frame = 0U;
    uint32_t last_rf_frame_count = 0U;

    while (true) {
        const uint32_t current_rf_frame_count = rf_receiver_frame_count();
        if (current_rf_frame_count != last_rf_frame_count) {
            last_rf_frame_count = current_rf_frame_count;
            rf_reaction_frame = 1U;
        }

        for (uint16_t x = 0; x < CONFIG_LED_MATRIX_WIDTH; ++x) {
            for (uint16_t y = 0; y < CONFIG_LED_MATRIX_HEIGHT; ++y) {
                uint8_t red;
                uint8_t green;
                uint8_t blue;
                color_wheel((uint8_t) (phase + x * 7U + y * 5U),
                            &red, &green, &blue);
                ESP_ERROR_CHECK(led_matrix_set_xy(x, y, red, green, blue));
            }
        }

        /* Each captured RF frame creates a bright expanding wave. */
        if (rf_reaction_frame > 0U) {
            const int center_x = CONFIG_LED_MATRIX_WIDTH / 2;
            const int center_y = CONFIG_LED_MATRIX_HEIGHT / 2;
            const int wave_radius = rf_reaction_frame * 5;
            const int previous_radius = wave_radius - 5;
            for (uint16_t x = 0; x < CONFIG_LED_MATRIX_WIDTH; ++x) {
                for (uint16_t y = 0; y < CONFIG_LED_MATRIX_HEIGHT; ++y) {
                    const int distance = abs((int) x - center_x) +
                                         abs((int) y - center_y) * 4;
                    if (distance >= previous_radius &&
                        distance < wave_radius) {
                        ESP_ERROR_CHECK(led_matrix_set_xy(x, y, 255, 255, 255));
                    }
                }
            }
            rf_reaction_frame++;
            if (rf_reaction_frame > 8U) {
                rf_reaction_frame = 0U;
            }
        }

        /* Top-left pixel: green means a frame arrived recently; red means idle. */
        if (rf_receiver_signal_recent()) {
            ESP_ERROR_CHECK(led_matrix_set_xy(0, 0, 0, 255, 0));
        } else {
            ESP_ERROR_CHECK(led_matrix_set_xy(0, 0, 32, 0, 0));
        }
        ESP_ERROR_CHECK(led_matrix_refresh());
        phase += 2U;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

esp_err_t led_matrix_init(void)
{
    const led_strip_config_t strip_config = {
        .strip_gpio_num = CONFIG_LED_DATA_GPIO,
        .max_leds = CONFIG_LED_MATRIX_WIDTH * CONFIG_LED_MATRIX_HEIGHT,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags.invert_out = false,
    };
    const led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 64,
        .flags.with_dma = true,
    };

    ESP_RETURN_ON_ERROR(led_strip_new_rmt_device(&strip_config, &rmt_config,
                                                  &s_strip),
                        TAG, "failed to create WS281x RMT driver");
    ESP_LOGI(TAG, "WS281x matrix ready: %dx%d, GRB, GPIO %d",
             CONFIG_LED_MATRIX_WIDTH,
             CONFIG_LED_MATRIX_HEIGHT,
             CONFIG_LED_DATA_GPIO);
    return led_strip_clear(s_strip);
}

esp_err_t led_matrix_fill(uint8_t red, uint8_t green, uint8_t blue)
{
    for (uint16_t x = 0; x < CONFIG_LED_MATRIX_WIDTH; ++x) {
        for (uint16_t y = 0; y < CONFIG_LED_MATRIX_HEIGHT; ++y) {
            ESP_RETURN_ON_ERROR(led_matrix_set_xy(x, y, red, green, blue),
                                TAG, "failed to set pixel");
        }
    }
    return led_matrix_refresh();
}

esp_err_t led_matrix_set_xy(uint16_t x, uint16_t y,
                            uint8_t red, uint8_t green, uint8_t blue)
{
    if (s_strip == NULL || x >= CONFIG_LED_MATRIX_WIDTH ||
        y >= CONFIG_LED_MATRIX_HEIGHT) {
        return ESP_ERR_INVALID_ARG;
    }

    return led_strip_set_pixel(s_strip, matrix_index(x, y),
                               scale_brightness(red),
                               scale_brightness(green),
                               scale_brightness(blue));
}

esp_err_t led_matrix_refresh(void)
{
    if (s_strip == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return led_strip_refresh(s_strip);
}

esp_err_t led_matrix_start_demo(void)
{
    if (s_strip == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xTaskCreate(matrix_demo_task, "matrix_demo", 4096, NULL, 4, NULL) !=
        pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "matrix demo started");
    return ESP_OK;
}
