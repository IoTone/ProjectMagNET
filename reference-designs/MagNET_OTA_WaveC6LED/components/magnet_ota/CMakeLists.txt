idf_component_register(
    SRCS "magnet_ota.c" "transport_ip.c" "ota_verify.c" "tweetnacl.c" "nacl_glue.c"
    INCLUDE_DIRS "include"
    REQUIRES magnet_cfg forth esp_http_client json esp_system mbedtls
)
# TweetNaCl is third-party and trips -Werror on shadowed decls and sign
# comparisons. Quarantine the relaxation to that one file rather than loosening
# the whole component's warnings.
set_source_files_properties(tweetnacl.c PROPERTIES COMPILE_FLAGS
    "-Wno-shadow -Wno-sign-compare -Wno-unused-parameter -Wno-char-subscripts")
