# The Storage facade (design 9). Dual-mode: an IDF component on the
# target, a static library on the host.
#
# worker.h is public here because the constructor takes the StorageWorker
# (design 4). The NVS and FATFS backends arrive at Milestones 3 and 5 and
# will be firmware-only, behind this same interface.

set(STORAGE_SRCS
    src/nn20clock_storage.c
)

if(ESP_PLATFORM)
    idf_component_register(
        SRCS ${STORAGE_SRCS}
        INCLUDE_DIRS "include"
        REQUIRES nn20clock_platform nn20clock_worker nn20clock_alarm
        PRIV_REQUIRES nn20clock_reqpool nn20clock_brightness nvs_flash
    )
    return()
endif()

# ----------------------------------------------------------- host build --

add_library(nn20clock_storage STATIC ${STORAGE_SRCS})

target_include_directories(nn20clock_storage
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(nn20clock_storage
    PUBLIC  nn20clock_platform nn20clock_worker nn20clock_alarm
    PRIVATE nn20clock_reqpool nn20clock_brightness
)

target_compile_options(nn20clock_storage PRIVATE
    -Wall -Wextra -Wpedantic -Wshadow -Werror
)
