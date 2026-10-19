idf_component_register(
    SRCS
        "motion.cpp"
        "face_input.cpp"
    INCLUDE_DIRS "include"
)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)
