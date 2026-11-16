# esp32emu's components for a project's EXTRA_COMPONENT_DIRS. Include it in
# the project's CMakeLists.txt before ESP-IDF's project.cmake:
#
#   include("${CMAKE_CURRENT_LIST_DIR}/../esp32emu/components.cmake")
#
# components/esp_lcd and components/fatfs are ESP-IDF 5.5.4's with esp32emu's
# changes; with ESP-IDF 6 they don't build, and idf6/esp_lcd and idf6/fatfs
# (ESP-IDF 6.1.1's, the same changes) take their place. Projects that still
# name components/ directly build with ESP-IDF 5 only.
include($ENV{IDF_PATH}/tools/cmake/version.cmake)

if(IDF_VERSION_MAJOR GREATER_EQUAL 6)
    file(GLOB emu_dirs LIST_DIRECTORIES true "${CMAKE_CURRENT_LIST_DIR}/components/*")
    foreach(dir ${emu_dirs})
        get_filename_component(name "${dir}" NAME)
        if(EXISTS "${dir}/CMakeLists.txt" AND NOT name STREQUAL "esp_lcd" AND NOT name STREQUAL "fatfs")
            list(APPEND EXTRA_COMPONENT_DIRS "${dir}")
        endif()
    endforeach()
    list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/idf6/esp_lcd"
                                     "${CMAKE_CURRENT_LIST_DIR}/idf6/fatfs")
else()
    list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/components")
endif()
