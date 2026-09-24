# The SD card (design 9's SD side). FIRMWARE ONLY - SDMMC and FATFS.
#
# The rules this enforces about media names and extensions live in
# nn20clock_media, which is dual-mode and tested on the host. Only the
# card handling is here.

if(NOT ESP_PLATFORM)
    return()
endif()

idf_component_register(
    SRCS "src/nn20clock_sd.c"
    INCLUDE_DIRS "include"
    REQUIRES nn20clock_platform nn20clock_media
    PRIV_REQUIRES fatfs sdmmc esp_driver_sdmmc vfs
)
