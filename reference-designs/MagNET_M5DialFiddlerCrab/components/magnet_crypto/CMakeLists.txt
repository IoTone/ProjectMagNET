idf_component_register(
    SRCS "magnet_crypto.c" "tweetnacl.c" "nacl_glue.c"
    INCLUDE_DIRS "include" "."
    PRIV_REQUIRES esp_hw_support
)

# TweetNaCl is third-party and trips -Werror on shadowed decls and sign
# comparisons. Quarantine the relaxation to that one file rather than
# loosening the whole component's warnings.
set_source_files_properties(tweetnacl.c PROPERTIES COMPILE_FLAGS
    "-Wno-shadow -Wno-sign-compare -Wno-unused-parameter -Wno-char-subscripts")
