idf_component_register(
    SRCS
        "si12t_touch.cpp"
        "io_expander_py32.cpp"
    INCLUDE_DIRS "include"
    REQUIRES
        m5stack__M5Unified
        tl_expected
)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)
