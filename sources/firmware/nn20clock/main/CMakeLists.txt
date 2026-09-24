idf_component_register(
    SRCS "nn20clock_app_main.c"
    INCLUDE_DIRS "."
    # nn20clock_app pulls in the workers, storage, timer, and manager; it
    # is the only component app_main should need to know about.
    REQUIRES nn20clock_platform nn20clock_app
    # esp_flash.h, for the boot-time flash size report. esp_chip_info.h and
    # esp_heap_caps.h come in with the default requirements.
    # esp_app_format, for the version the banner reports.
    PRIV_REQUIRES spi_flash esp_app_format
)
