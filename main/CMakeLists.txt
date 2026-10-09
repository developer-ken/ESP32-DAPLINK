idf_component_register(SRCS "swo.c" "main.cpp"
                             "dap.c"
                             "swd_jtag.c"
                             "status_led.c"
                             "usb_descriptors.c"
                             "msc_disk.c"
                             "wifi_bridge.c"
                             "wifi_config.c"
                             "remote_bitbang.c"
                             "cmsis_dap_tcp.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES driver esp_wifi esp_netif esp_event nvs_flash
                                  fatfs wear_levelling vfs esp_partition
                                  esp_rom lwip esp_tinyusb mdns)
