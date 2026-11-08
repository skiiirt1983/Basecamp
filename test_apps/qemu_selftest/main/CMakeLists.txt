idf_component_register(SRCS "selftest_main.c"
                       PRIV_INCLUDE_DIRS "../../../common/include"
                       PRIV_REQUIRES app_update esp_partition nvs_flash esp_hw_support)

if(DEFINED SELFTEST_DEEP_SLEEP)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SELFTEST_DEEP_SLEEP=${SELFTEST_DEEP_SLEEP})
endif()
