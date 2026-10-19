idf_component_register(
    SRCS "servo_limits.cpp"
    INCLUDE_DIRS "include"
    REQUIRES json
)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)
