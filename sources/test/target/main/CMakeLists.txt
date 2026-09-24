# The suite sources live one level up in test/, unchanged - the whole
# point is that the target runs the same tests the host does. They are
# listed here rather than globbed so adding a suite is a deliberate act.
#
# test_nn20clock_sd.c is the exception: it is the only suite that runs
# here and NOT on the host, because nn20clock_sd is SDMMC and FATFS and
# there is nothing underneath it to fake. It is not in
# test/CMakeLists.txt for that reason.

idf_component_register(
    SRCS "nn20clock_target_test_main.c"
         "../../test_nn20clock_workers.c"
         "../../test_nn20clock_timer.c"
         "../../test_nn20clock_timefmt.c"
         "../../test_nn20clock_alarm.c"
         "../../test_nn20clock_media.c"
         "../../test_nn20clock_sd.c"
         "../../test_nn20clock_avi.c"
         "../../test_nn20clock_ringbuf.c"
         "../../test_nn20clock_storage.c"
         "../../test_nn20clock_ui.c"
         "../../test_nn20clock_ota.c"
         "../../test_nn20clock_manager.c"
         "../../test_nn20clock_app.c"
    INCLUDE_DIRS "." "../.."
    REQUIRES nn20clock_platform nn20clock_workers nn20clock_timer
             nn20clock_timefmt nn20clock_alarm nn20clock_media nn20clock_sd
             nn20clock_avi nn20clock_ringbuf nn20clock_storage
             nn20clock_ui nn20clock_ota nn20clock_manager
             nn20clock_app
    # The SD suite sets the FAT hidden bit with f_chmod(), which has no
    # POSIX equivalent - the VFS carries the attribute in neither
    # direction, which is the whole reason the product reads it through
    # FatFs too.
    PRIV_REQUIRES fatfs
)

# test_util.h defines main() via TEST_MAIN. On the target app_main() is
# the entry point instead, so each suite's main() is renamed out of the
# way and called from nn20clock_target_test_main.c.
set_source_files_properties("../../test_nn20clock_workers.c"
    PROPERTIES COMPILE_DEFINITIONS "main=nn20clock_test_main_workers")
set_source_files_properties("../../test_nn20clock_timer.c"
    PROPERTIES COMPILE_DEFINITIONS "main=nn20clock_test_main_timer")
set_source_files_properties("../../test_nn20clock_timefmt.c"
    PROPERTIES COMPILE_DEFINITIONS "main=nn20clock_test_main_timefmt")
set_source_files_properties("../../test_nn20clock_alarm.c"
    PROPERTIES COMPILE_DEFINITIONS "main=nn20clock_test_main_alarm")
set_source_files_properties("../../test_nn20clock_media.c"
    PROPERTIES COMPILE_DEFINITIONS "main=nn20clock_test_main_media")
set_source_files_properties("../../test_nn20clock_sd.c"
    PROPERTIES COMPILE_DEFINITIONS "main=nn20clock_test_main_sd")
set_source_files_properties("../../test_nn20clock_avi.c"
    PROPERTIES COMPILE_DEFINITIONS "main=nn20clock_test_main_avi")
set_source_files_properties("../../test_nn20clock_ringbuf.c"
    PROPERTIES COMPILE_DEFINITIONS "main=nn20clock_test_main_ringbuf")
set_source_files_properties("../../test_nn20clock_storage.c"
    PROPERTIES COMPILE_DEFINITIONS "main=nn20clock_test_main_storage")
set_source_files_properties("../../test_nn20clock_ui.c"
    PROPERTIES COMPILE_DEFINITIONS "main=nn20clock_test_main_ui")
set_source_files_properties("../../test_nn20clock_ota.c"
    PROPERTIES COMPILE_DEFINITIONS "main=nn20clock_test_main_ota")
set_source_files_properties("../../test_nn20clock_manager.c"
    PROPERTIES COMPILE_DEFINITIONS "main=nn20clock_test_main_manager")
set_source_files_properties("../../test_nn20clock_app.c"
    PROPERTIES COMPILE_DEFINITIONS "main=nn20clock_test_main_app")
