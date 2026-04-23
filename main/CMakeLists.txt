idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        nvs_flash
        esp_wifi
        esp_event
        esp_netif
        esp_lcd
        driver
        heap
        log
        freertos
        vfs
        fatfs
        sdmmc
        esp_rainmaker
        rmaker_common
        json
        mqtt
        esp-tls
        tcp_transport
        protocomm
        espressif__network_provisioning
        bt
        esp_timer
)
