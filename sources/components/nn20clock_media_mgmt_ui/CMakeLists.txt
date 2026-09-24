# The media management screen (design 11, MediaManagementUi). FIRMWARE
# ONLY - it draws with LVGL, and everything it reports comes from
# components that only exist on the device: the FTP server, the radio,
# and the card.
#
# There is no host test for this one and there should not be: it holds
# no logic of its own. Every number on it is read straight from an
# accessor that is tested where it lives.

if(NOT ESP_PLATFORM)
    return()
endif()

idf_component_register(
    SRCS "src/nn20clock_media_mgmt_ui.c"
    INCLUDE_DIRS "include"
    REQUIRES nn20clock_platform nn20clock_ui nn20clock_manager
             nn20clock_ftp nn20clock_net nn20clock_sd nn20clock_media
             nn20clock_fonts
)
