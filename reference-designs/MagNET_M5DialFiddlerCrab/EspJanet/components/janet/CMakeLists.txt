idf_component_register(
    SRCS "janet.c"
    INCLUDE_DIRS "."
    REQUIRES esp_timer
)

# Most configuration is done directly in janet.h (EspJanet config block).
# These overrides are for values that have #ifndef guards in the header.
target_compile_definitions(${COMPONENT_LIB} PUBLIC
    JANET_RECURSION_GUARD=256
    JANET_STACK_MAX=8192
)

# Suppress warnings in third-party amalgamation code
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-unused-function
    -Wno-unused-variable
    -Wno-sign-compare
    -Wno-format
    -Wno-implicit-fallthrough
    -Wno-old-style-declaration
    -Wno-missing-braces
    -Wno-type-limits
    -Wno-maybe-uninitialized
)
