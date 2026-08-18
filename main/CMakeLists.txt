idf_component_register(
    SRCS
        "app_main.c"
        "rf_receiver.c"
        "led_matrix.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_driver_gpio
        esp_driver_rmt
        led_strip
)
