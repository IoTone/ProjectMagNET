# claw_mdns is OPTIONAL — only compiled when CLAW_ENABLE_MDNS is set.
# To enable: add -DCLAW_ENABLE_MDNS=1 to build_flags and add espressif/mdns
# to your idf_component.yml dependencies.

if(DEFINED CLAW_ENABLE_MDNS)
    idf_component_register(
        SRCS "claw_mdns.c"
        INCLUDE_DIRS "."
        REQUIRES mdns
    )
else()
    # Register as empty component so CMake doesn't complain about missing dir
    idf_component_register()
endif()
