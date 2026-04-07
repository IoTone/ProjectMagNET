# craw_mdns is OPTIONAL — only compiled when CRAW_ENABLE_MDNS is set.
# To enable: add -DCRAW_ENABLE_MDNS=1 to build_flags and add espressif/mdns
# to your idf_component.yml dependencies.

if(DEFINED CRAW_ENABLE_MDNS)
    idf_component_register(
        SRCS "craw_mdns.c"
        INCLUDE_DIRS "."
        REQUIRES mdns
    )
else()
    # Register as empty component so CMake doesn't complain about missing dir
    idf_component_register()
endif()
