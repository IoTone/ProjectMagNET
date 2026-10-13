/* sdkconfig.h — the one IDF config symbol MagNET reads (SYSINFO target name).
 * Zephyr's own CONFIG_* come from autoconf.h, which is always force-included. */
#ifndef MN_COMPAT_SDKCONFIG_H
#define MN_COMPAT_SDKCONFIG_H
#define CONFIG_IDF_TARGET "efr32mg24"
#endif
