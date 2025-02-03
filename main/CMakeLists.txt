idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       EMBED_FILES "index.html"
                       REQUIRES driver esp_wifi nvs_flash esp_http_server led_strip)
