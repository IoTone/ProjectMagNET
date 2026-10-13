# Release builds sign OFFLINE (tools/keys/README.md). With -DMN_SIGN_OFFLINE=1
# and SB_CONFIG_BOOT_SIGNATURE_KEY_FILE pointing at the PUBLIC MCUboot key,
# MCUboot is built with that key embedded and the application is left
# unsigned (zephyr.bin) for tools/mcuboot_sign.py on the signing machine.
# Stock sysbuild hands the same key file to imgtool to sign the app, which
# would need the private half here. Sysbuild's own image defaults run after
# this file, so the override is appended to the app's IMAGE_CONF_SCRIPT list
# (run in order at image configure time) instead of being set here.
if(MN_SIGN_OFFLINE)
  set_property(TARGET ${DEFAULT_IMAGE} APPEND PROPERTY IMAGE_CONF_SCRIPT
               ${CMAKE_CURRENT_LIST_DIR}/sysbuild/sign_offline.cmake)
endif()
