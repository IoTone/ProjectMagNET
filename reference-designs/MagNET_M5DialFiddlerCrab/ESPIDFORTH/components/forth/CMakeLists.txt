idf_component_register(
    SRCS "forth_core.cpp"
    INCLUDE_DIRS "."
    REQUIRES esp_timer driver
)

# Suppress warnings in third-party code
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-unused-function
    -Wno-unused-variable
    -Wno-sign-compare
    -Wno-write-strings
    -Wno-missing-field-initializers
)
