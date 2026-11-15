# QEMU self-test of the launcher's slot writer (main/swriter.c), metadata store
# (main/meta.c) and slot scan (main/slots.c) against emulated flash.
# Run: tools/qemu_swriter_test.sh
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(basecamp_swtest)
