idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES fatfs esp_driver_sdspi esp_driver_i2c sdmmc vfs stick_s3_board
)
