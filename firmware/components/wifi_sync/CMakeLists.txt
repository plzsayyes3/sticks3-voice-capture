idf_component_register(
    SRCS "wifi_sync.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_netif esp_event nvs_flash esp_http_client esp-tls recording_store
)
