idf_component_register(
    SRCS "recording_store.c"
    INCLUDE_DIRS "include"
    REQUIRES fatfs wear_levelling esp_partition esp_system
)
