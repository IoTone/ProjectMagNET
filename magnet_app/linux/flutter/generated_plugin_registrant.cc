//
//  Generated file. Do not edit.
//

// clang-format off

#include "generated_plugin_registrant.h"

#include <audioplayers_linux/audioplayers_linux_plugin.h>
#include <cbl_flutter_ce/cbl_flutter_ce.h>

void fl_register_plugins(FlPluginRegistry* registry) {
  g_autoptr(FlPluginRegistrar) audioplayers_linux_registrar =
      fl_plugin_registry_get_registrar_for_plugin(registry, "AudioplayersLinuxPlugin");
  audioplayers_linux_plugin_register_with_registrar(audioplayers_linux_registrar);
  g_autoptr(FlPluginRegistrar) cbl_flutter_ce_registrar =
      fl_plugin_registry_get_registrar_for_plugin(registry, "CblFlutterCe");
  cbl_flutter_ce_register_with_registrar(cbl_flutter_ce_registrar);
}
