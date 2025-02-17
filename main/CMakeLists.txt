idf_component_register(
    SRC_DIRS "."
    INCLUDE_DIRS "."
    PRIV_REQUIRES app_update esp_driver_gpio esp_http_server esp_ringbuf
                  esp_wifi nvs_flash soc usb wilma wpa_supplicant
    EMBED_TXTFILES "../html/status.html"
                   "../html/wifi.html"
    LDFRAGMENTS "linker_fragments.lf"
)
