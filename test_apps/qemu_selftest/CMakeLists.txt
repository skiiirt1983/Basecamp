# Headless QEMU self-test for the Basecamp boot-slot selection.
# The SAME binary is flashed into factory, ota_0, ota_1 and ota_2; its role is
# decided by the partition it runs from. Uses the real custom bootloader from
# ../../bootloader_components. Run: tools/qemu_selftest.sh
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(BOOTLOADER_EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../bootloader_components" APPEND)
project(basecamp_selftest)
