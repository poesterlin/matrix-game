#include "esp_err.h"
#include "esp_log.h"

#include "led_matrix.h"
#include "rf_receiver.h"

static const char *TAG = "rf_led_matrix";

void app_main(void)
{
    ESP_LOGI(TAG, "starting firmware");
    ESP_LOGI(TAG, "matrix=%dx%d brightness=%d%% RF GPIO=%d LED GPIO=%d",
             CONFIG_LED_MATRIX_WIDTH,
             CONFIG_LED_MATRIX_HEIGHT,
             CONFIG_LED_BRIGHTNESS,
             CONFIG_RF_INPUT_GPIO,
             CONFIG_LED_DATA_GPIO);

    ESP_ERROR_CHECK(led_matrix_init());
    ESP_ERROR_CHECK(rf_receiver_init());
    ESP_ERROR_CHECK(led_matrix_fill(0, 0, 0));
    ESP_ERROR_CHECK(led_matrix_start_demo());
    ESP_LOGI(TAG, "startup complete");
}
