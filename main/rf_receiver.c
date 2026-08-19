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
static volatile uint32_t s_last_payload;
static TickType_t s_last_trigger_tick;

/* Your Button's Unique 20-bit Hardware ID Mask (0x572E1) */
#define TARGET_BUTTON_ID_MASK 0x572E1U

enum {
    RF_FRAME_DEBOUNCE_MS = 150,
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

            /* Decode EV1527 24-bit RF protocol payload */
            uint32_t payload_code = 0;
            int decoded_bits = 0;

            for (size_t i = 0; i < event.num_symbols; ++i) {
                uint32_t d0 = event.received_symbols[i].duration0;
                uint32_t d1 = event.received_symbols[i].duration1;

                /* Bit '1': High ~1700us (1100-2200), Low ~575us (300-950) */
                if (d0 > 1100 && d0 < 2200 && d1 > 300 && d1 < 950) {
                    payload_code = (payload_code << 1) | 1U;
                    decoded_bits++;
                }
                /* Bit '0': High ~575us (300-950), Low ~1725us (1100-2200) */
                else if (d0 > 300 && d0 < 950 && d1 > 1100 && d1 < 2200) {
                    payload_code = (payload_code << 1) | 0U;
                    decoded_bits++;
                }
            }

            /* Filter strictly for your button's hardware ID (0x572E1) */
            const bool is_target_button = (decoded_bits >= 18) &&
                                           ((payload_code & 0xFFFFF) == TARGET_BUTTON_ID_MASK);

            const bool outside_debounce =
                (s_last_trigger_tick == 0) ||
                ((now - s_last_trigger_tick) >= pdMS_TO_TICKS(RF_FRAME_DEBOUNCE_MS));

            if (is_target_button) {
                s_last_frame_tick = now;
                s_last_payload = payload_code;

                ESP_LOGI(TAG, "MATCH! Valid Button Press (ID: 0x%05X, Payload: 0x%06X)",
                         TARGET_BUTTON_ID_MASK, (unsigned) payload_code);

                if (outside_debounce) {
                    s_last_trigger_tick = now;
                    s_frame_count++;
                }
            } else if (decoded_bits >= 10) {
                ESP_LOGD(TAG, "Ignored RF interference/other device: 0x%06X (%d bits)",
                         (unsigned) payload_code, decoded_bits);
            }

            /* Re-arm capture */
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

uint32_t rf_receiver_last_payload(void)
{
    return s_last_payload;
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

    ESP_LOGI(TAG, "433 MHz locked decoder ready on GPIO %d (Target ID: 0x%05X)",
             CONFIG_RF_INPUT_GPIO, TARGET_BUTTON_ID_MASK);
    return ESP_OK;
}
