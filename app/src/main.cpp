// Sims 3 Switch port: app entry.
//
// Game files go in <folder of the .nro>/game (the extracted disc, with default.xex at its top).
#include "sims3_init.h"

#include <rex/filesystem.h>
#include <rex/rex_app.h>
#include <rex/runtime.h>
#include <rex/system/gpu_plugin.h>

#include "deko3d_graphics.h"
#include "sims3_build.h"  // written by build_app.sh: SIMS3_TITLE_UPDATE


// Sims3.xex's recompiled code, renamed at compile time (CMakeLists.txt) so it can share the NRO with
// default.xex's.
extern "C" void Sims3Dll_Register(rex::runtime::IModuleRegistrar* registrar);
extern "C" const rex::PPCImageInfo* Sims3Dll_GetImageInfo();

namespace rex::platform {
void RegisterStaticLibrarySymbol(const char* library, const char* symbol, void* address);
}

// The SDK's Switch loader expects the Xenos GPU plugin to be linked in. This port supplies its own
// graphics system and never asks for it, so version 0 makes that path fail cleanly if it is reached.
extern "C" uint32_t rex_gpu_abi_version(void) {
  return 0;
}
extern "C" rex::system::IGraphicsSystem* rex_gpu_create(uint32_t, const rex::system::GpuCreateInfo*) {
  return nullptr;
}

namespace {

class Sims3App final : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext& ctx) {
    // Codegen names this module "sims3_Sims3" in module_registry.cpp.
    rex::platform::RegisterStaticLibrarySymbol("sims3_Sims3", "ReXModule_Register",
                                               reinterpret_cast<void*>(&Sims3Dll_Register));
    rex::platform::RegisterStaticLibrarySymbol("sims3_Sims3", "ReXModule_GetImageInfo",
                                               reinterpret_cast<void*>(&Sims3Dll_GetImageInfo));
    return std::unique_ptr<Sims3App>(new Sims3App(ctx, "sims3", PPCImageConfig));
  }


 protected:
  void OnPreSetup(rex::RuntimeConfig& config) override {
    config.graphics = sims3::CreateDeko3dGraphicsSystem();
  }

  void OnConfigurePaths(rex::PathConfig& paths) override {
    const auto root = rex::filesystem::GetExecutableFolder();
    paths.config_path = root / "sims3.toml";
    if (paths.game_data_root.empty()) paths.game_data_root = root / "game";
#if SIMS3_TITLE_UPDATE
    // Title update 1: <folder of the .nro>/update holds globalpatch.idx, GameplayRelease/ and the update's
    // Sims3.xexp and PrivLoader.xexp, mounted as update: (the game reads UPDATE:\globalpatch.idx).
    if (paths.update_data_root.empty()) paths.update_data_root = root / "update";
#endif
  }
};

}  // namespace

REX_DEFINE_APP(sims3, Sims3App::Create)
