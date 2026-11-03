// DualSense effects for Dusklight: sound-driven haptics, rumble modes, adaptive triggers,
// lightbar, player LEDs and mic LED. See haptics.cpp and controller.cpp.
#include "controller.hpp"
#include "haptics.hpp"
#include "settings.hpp"

#include "mods/service.hpp"
#include "mods/svc/config.h"
#include "mods/svc/hook.h"
#include "mods/svc/host.h"
#include "mods/svc/log.h"
#include "mods/svc/ui.h"

DEFINE_MOD();
IMPORT_SERVICE(LogService, svc_log);
IMPORT_SERVICE(ConfigService, svc_config);
IMPORT_SERVICE(UiService, svc_ui);
IMPORT_SERVICE(HookService, svc_hook);
IMPORT_SERVICE(HostService, svc_host);

extern "C" {

MOD_EXPORT ModResult mod_initialize(ModError* error) {
    if (ds::settings_register(error) != MOD_OK) {
        return MOD_ERROR;
    }
    if (ds::haptics_init(svc_host->native_dir(mod_ctx), error) != MOD_OK || ds::controller_init(error) != MOD_OK) {
        return MOD_ERROR;
    }
    svc_log->info(mod_ctx, "DualSense effects initialized");
    return MOD_OK;
}

MOD_EXPORT ModResult mod_update(ModError*) {
    ds::settings_refresh();
    ds::haptics_update();
    ds::controller_update();
    return MOD_OK;
}

MOD_EXPORT ModResult mod_shutdown(ModError*) {
    ds::controller_shutdown();
    ds::haptics_shutdown();
    return MOD_OK;
}
}
