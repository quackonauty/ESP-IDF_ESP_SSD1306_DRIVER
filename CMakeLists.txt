idf_component_register(
    SRCS "esp_ssd1306_driver.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        esp_driver_i2c
        freertos
)