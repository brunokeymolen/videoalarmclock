# Single-consumer task worker, vendored unchanged from
# ~/coding-style-and-examples/esp32/worker.
#
# Already dual-platform in its own right: worker_port.c is a FreeRTOS task
# pinned with xTaskCreatePinnedToCore() under ESP_PLATFORM and a pthread
# with CPU affinity otherwise. That is why it needs no shim from
# nn20clock_platform - it predates the seam and draws its own.

set(WORKER_SRCS
    worker.c
    worker_port.c
)

if(ESP_PLATFORM)
    idf_component_register(
        SRCS ${WORKER_SRCS}
        INCLUDE_DIRS "."
        PRIV_REQUIRES freertos
    )
    return()
endif()

# ----------------------------------------------------------- host build --

add_library(nn20clock_worker STATIC ${WORKER_SRCS})

target_include_directories(nn20clock_worker
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
)

# pthread_setname_np / CPU affinity attributes need _GNU_SOURCE.
target_compile_definitions(nn20clock_worker PRIVATE _GNU_SOURCE)

find_package(Threads REQUIRED)
target_link_libraries(nn20clock_worker PUBLIC Threads::Threads)
