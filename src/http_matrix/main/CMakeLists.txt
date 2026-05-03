idf_component_register(
    SRCS "http_matrix_main.c" "glcdfont.c" "cjk8x8.c"
    INCLUDE_DIRS "."
    REQUIRES esp_http_server esp_wifi esp_netif esp_event nvs_flash led_strip
)
