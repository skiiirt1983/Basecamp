# Dummy OTA app used to prove Basecamp slot selection before the real bins.
# Build three variants:  ../../tools/build_dummies.sh   (or see README)
cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED DUMMY_ID)
    set(DUMMY_ID "a")
endif()
set(PROJECT_VER "dummy-${DUMMY_ID}-1.0")
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components")
set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/../../sdkconfig.defaults;${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(basecamp_dummy_${DUMMY_ID})
idf_build_set_property(COMPILE_DEFINITIONS "DUMMY_ID=\"${DUMMY_ID}\"" APPEND)
