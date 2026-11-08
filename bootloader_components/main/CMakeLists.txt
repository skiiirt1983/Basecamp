# Overrides the stock 2nd-stage bootloader "main" component.
# The custom entry point decides between the factory launcher and ota_0..2
# based on the selection word in RTC_CNTL_STORE0_REG (see common/include).
idf_component_register(SRCS "bootloader_start.c"
                       INCLUDE_DIRS "../../common/include"
                       REQUIRES bootloader bootloader_support)

# Use the default bootloader linker scripts
idf_build_get_property(scripts BOOTLOADER_LINKER_SCRIPT)
target_linker_script(${COMPONENT_LIB} INTERFACE "${scripts}")
