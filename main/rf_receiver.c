#include "rf_receiver.h"

#include "driver/rmt_rx.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "board_config.h"

static const char *TAG = "rf_receiver";
static rmt_channel_handle_t s_rx_channel;
static QueueHandle_t s_rx_done_queue;
static rmt_symbol_word_t s_symbols[CONFIG_RF_CAPTURE_SYMBOLS * 2];
static volatile TickType_t s_last_frame_tick;
static volatile uint32_t s_frame_count;
static TickType_t s_last_trigger_tick;

enum {
    RF_MIN_FRAME_SYMBOLS = 3,
    RF_FRAME_DEBOUNCE_MS = 100,
};

static bool IRAM_ATTR on_receive_done(rmt_channel_handle_t channel,
                                      const rmt_rx_done_event_data_t *event,
                                      void *user_data)
{
    (void) channel;
    (void) user_data;
    BaseType_t high_task_woken = pdFALSE;
    xQueueSendFromISR(s_rx_done_queue, event, &high_task_woken);
    return high_task_woken == pdTRUE;
}

static void rf_decode_task(void *argument)
{
    (void) argument;
    rmt_rx_done_event_data_t event;
    const rmt_receive_config_t receive_config = {
        .signal_range_min_ns = CONFIG_RF_PULSE_MIN_US * 1000U,
        .signal_range_max_ns = CONFIG_RF_PULSE_MAX_US * 1000U,
    };

    esp_err_t err = rmt_receive(s_rx_channel, s_symbols, sizeof(s_symbols),
                                &receive_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to start RF capture: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    while (true) {
        if (xQueueReceive(s_rx_done_queue, &event, portMAX_DELAY) == pdTRUE) {
            const TickType_t now = xTaskGetTickCount();
            const bool valid_frame = event.num_symbols >= RF_MIN_FRAME_SYMBOLS;
            const bool outside_debounce =
                (s_last_trigger_tick == 0) ||
                ((now - s_last_trigger_tick) >= pdMS_TO_TICKS(RF_FRAME_DEBOUNCE_MS));

            /* Ignore the single long/noisy pulses produced by idle receivers. */
            if (valid_frame) {
                s_last_frame_tick = now;
                if (outside_debounce) {
                    s_last_trigger_tick = now;
                    s_frame_count++;
                }
            }

            if (!valid_frame) {
                goto rearm_capture;
            }

            uint32_t total_us = 0;
            size_t logged = event.num_symbols * 2U;
            if (logged > 16U) {
                logged = 16U;
            }

            for (size_t i = 0; i < event.num_symbols; ++i) {
                total_us += event.received_symbols[i].duration0;
                total_us += event.received_symbols[i].duration1;
                if (i * 2U < logged) {
                    ESP_LOGI(TAG, "pulse[%u] level=%u width=%uus",
                             (unsigned) (i * 2U),
                             (unsigned) event.received_symbols[i].level0,
                             (unsigned) event.received_symbols[i].duration0);
                }
                if (i * 2U + 1U < logged) {
                    ESP_LOGI(TAG, "pulse[%u] level=%u width=%uus",
                             (unsigned) (i * 2U + 1U),
                             (unsigned) event.received_symbols[i].level1,
                             (unsigned) event.received_symbols[i].duration1);
                }
            }
            ESP_LOGI(TAG, "raw frame: %u symbols, %uus total",
                     (unsigned) event.num_symbols, (unsigned) total_us);

            /* Re-arm only after the normal task has consumed the frame. */
rearm_capture:
            err = rmt_receive(s_rx_channel, s_symbols, sizeof(s_symbols),
                              &receive_config);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "failed to re-arm RF capture: %s",
                         esp_err_to_name(err));
                vTaskDelete(NULL);
                return;
            }
        }
    }
}

bool rf_receiver_signal_recent(void)
{
    const TickType_t now = xTaskGetTickCount();
    const TickType_t timeout = pdMS_TO_TICKS(1000);
    return s_last_frame_tick != 0 && (now - s_last_frame_tick) < timeout;
}

uint32_t rf_receiver_frame_count(void)
{
    return s_frame_count;
}

esp_err_t rf_receiver_init(void)
{
    const rmt_rx_channel_config_t channel_config = {
        .gpio_num = CONFIG_RF_INPUT_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 1000000,
        .mem_block_symbols = CONFIG_RF_CAPTURE_SYMBOLS,
        .intr_priority = 0,
        .flags.with_dma = true,
    };
    const rmt_rx_event_callbacks_t callbacks = {
        .on_recv_done = on_receive_done,
    };

    s_rx_done_queue = xQueueCreate(2, sizeof(rmt_rx_done_event_data_t));
    if (s_rx_done_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ESP_RETURN_ON_ERROR(rmt_new_rx_channel(&channel_config, &s_rx_channel),
                        TAG, "failed to create RF RMT RX channel");
    ESP_RETURN_ON_ERROR(rmt_rx_register_event_callbacks(s_rx_channel,
                                                        &callbacks, NULL),
                        TAG, "failed to register RF callback");
    ESP_RETURN_ON_ERROR(rmt_enable(s_rx_channel), TAG,
                        "failed to enable RF RMT RX channel");

    if (xTaskCreate(rf_decode_task, "rf_decode", 4096, NULL, 5, NULL) !=
        pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "433 MHz raw receiver ready on GPIO %d",
             CONFIG_RF_INPUT_GPIO);
    return ESP_OK;
}
