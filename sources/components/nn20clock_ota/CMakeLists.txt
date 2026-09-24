# Updating the clock over the network.
#
# Dual-mode, and only just: the firmware build gets the whole component,
# the host build gets nn20clock_ota_version.c and nothing else.
#
# Everything in nn20clock_ota.c writes app partitions, opens TLS
# sockets, or reboots, and there is no honest host equivalent of any of
# it - the same seam nn20clock_net sits on. But the version comparison
# is pure arithmetic over two strings, it decides whether the clock
# replaces its own firmware, and it fails silently in both directions.
# That belongs under `tools/test.sh`, so it is compiled where the tests
# can reach it.
#
# `app_update` is esp_ota_ops, `esp-tls` carries the root bundle, and
# `json` is ESP-IDF's cJSON, used for the release manifest and nothing
# else.

if(ESP_PLATFORM)
    idf_component_register(
        SRCS "src/nn20clock_ota.c"
             "src/nn20clock_ota_version.c"
        INCLUDE_DIRS "include"
        REQUIRES nn20clock_platform nn20clock_worker
        PRIV_REQUIRES app_update esp_http_client esp-tls esp_app_format
                      mbedtls json
    )
    return()
endif()

# ----------------------------------------------------------- host build --

add_library(nn20clock_ota STATIC src/nn20clock_ota_version.c)

target_include_directories(nn20clock_ota
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_options(nn20clock_ota PRIVATE
    -Wall -Wextra -Wpedantic -Wshadow -Werror
)
