idf_component_register(
    SRCS
        "scs_bus.cpp"
        "scs_servo.cpp"
    INCLUDE_DIRS "include"
    REQUIRES
        tl_expected
    PRIV_REQUIRES
        driver
        esp_timer
)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)
