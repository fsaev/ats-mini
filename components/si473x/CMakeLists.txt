idf_component_register(
    SRCS "src/si473x.cpp" "src/si473x_rds.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_i2c esp_driver_gpio
    PRIV_REQUIRES esp_timer esp_rom freertos log
)
