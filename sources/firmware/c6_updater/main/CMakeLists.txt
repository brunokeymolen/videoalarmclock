# The C6 image is built first by tools/c6-update.sh.
set(C6_IMAGE "${CMAKE_CURRENT_LIST_DIR}/../../c6_cp/build/c6_cp.bin")
if(NOT EXISTS "${C6_IMAGE}")
    message(FATAL_ERROR "${C6_IMAGE} is missing - build firmware/c6_cp "
                        "first, or run tools/c6-update.sh")
endif()

idf_component_register(SRCS "main.c"
                       EMBED_FILES "${C6_IMAGE}"
                       PRIV_REQUIRES nvs_flash esp_event esp_system)
