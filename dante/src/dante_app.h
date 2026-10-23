// dantes-inferno-nx - Dante's Inferno (Xbox 360, Title Update 2) on the Nintendo Switch.
//
// Where the game is, next to the NRO (sdmc:/switch/dantes-inferno-nx/ on the console), either way:
//   game/   the extracted disc (default.xex, bigfile0.viv, bigfile1.viv) and Title Update 2's default.xexp
//           (tools/stage_sd.py makes it from your ISO and title update); the runtime maps game: and d: to it;
//   god/    the Games on Demand package as the Xbox 360 hard drive has it (the header file and its .data folder);
//           dante_god.cpp mounts it as d: and loads its default.xex from god_xex/ (the runtime's game: then).
//
// The recompiled code is TU2: it only matches the image the title update patches, so default.xexp is required (the
// runtime applies it when it loads default.xex).

#pragma once

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/rex_app.h>
#include <rex/runtime.h>
#include <rex/system.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/xmemory.h>

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include "dante_cpu.h"
#include "dante_god.h"
#include "dante_gpu.h"

#if defined(__SWITCH__)
// The SDK's debug shortcuts (ZL + ZR + Down / Left / Right / R ...: captures, frame dumps, draw-skipping modes, stack
// sampling): off in a release whatever dante.toml says; ZL + ZR + Up, the FPS counter, stays.
REXCVAR_DECLARE(bool, switch_atajos_depuracion);
#endif

REXCVAR_DEFINE_STRING(dante_game, "", "Dante", "Carpeta del juego (disco extraido + default.xexp). Vacio: game/ junto al ejecutable.");

class DanteApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<DanteApp>(new DanteApp(ctx, "dante", PPCImageConfig));
  }

 protected:
  // No Xenos emulation: dante_gpu.cpp reads the game's command ring and draws with the deko3d renderer.
  void OnPreSetup(rex::RuntimeConfig& config) override {
    if (!config.graphics) config.graphics = dante::gpu::CreateGraphicsSystem();
  }

  // game_data_root is the folder with default.xex (ConstructRuntime checks it).
  void OnConfigurePaths(rex::PathConfig& paths) override {
    if (!paths.game_data_root.empty()) return;
    const std::string flag = REXCVAR_GET(dante_game);
    if (!flag.empty()) {
      paths.game_data_root = flag;
      return;
    }
    const auto folder = rex::filesystem::GetExecutableFolder();
    if (folder.empty()) return;
    std::error_code ec;
    if (std::filesystem::is_directory(folder / "god", ec)) {
      // A Games on Demand package: its executable is loaded from god_xex/ (dante_god.cpp fills it).
      std::filesystem::create_directories(folder / "god_xex", ec);
      paths.game_data_root = folder / "god_xex";
      return;
    }
    paths.game_data_root = folder / "game";
  }

  void OnPostInitLogging() override {
#if defined(__SWITCH__)
    REXCVAR_SET(switch_atajos_depuracion, false);
#endif
    dante::cpu::InformarSistema();
  }

  void OnLoadXexImage(std::string& xex_image) override {
    std::error_code ec;
    const auto root = game_data_root();
    bool god = false;
    try {
      const auto folder = rex::filesystem::GetExecutableFolder();
      god = dante::god::Montar(*runtime(), folder / "god", root, folder / "game");
    } catch (const std::exception& error) {
      Fail(std::string("Dante's Inferno: ") + error.what());
    }
    const std::vector<const char*> needed =
        god ? std::vector<const char*>{"default.xex"}
            : std::vector<const char*>{"default.xex", "default.xexp", "bigfile0.viv", "bigfile1.viv"};
    for (const char* name : needed) {
      if (!std::filesystem::is_regular_file(root / name, ec)) {
        Fail(fmt::format("Dante's Inferno: {} is missing from {}. Make the folder with tools/stage_sd.py (INSTALL.md).",
                         name, root.string()));
      }
    }
    REXLOG_INFO("dante: game at {}{}; loading {}", root.string(), god ? " and god/" : "", xex_image);
  }

  void OnPreLaunchModule() override {
    dante::cpu::Configurar();
    dante::god::DespuesDeCargar(*runtime());
    // The code is TU2's. Its guest setjmp (sub_82879110) only exists in the TU2 image: without it the title update
    // was not applied, and nothing would run right.
    auto* dispatcher = runtime()->function_dispatcher();
    if (!dispatcher || !dispatcher->GetFunction(0x82879110)) {
      Fail("Dante's Inferno: this build is for Title Update 2, and the loaded image is not TU2 (default.xexp?)");
    }
    // The save system's guest setjmp would switch fibers through a callback pointer when it is set; with the host
    // setjmp/longjmp of dantes_inferno_hooks.h that switch must not happen, so the pointer is cleared (TU2 address).
    uint8_t* membase = runtime()->memory()->virtual_membase();
    *reinterpret_cast<uint32_t*>(membase + 0x82CE68E4u) = 0u;
  }

 private:
  [[noreturn]] static void Fail(const std::string& msg) {
    REXLOG_ERROR("{}", msg);
    rex::ShowSimpleMessageBox(rex::SimpleMessageBoxType::Error, msg);
    rex::FlushLogging();
    std::_Exit(1);
  }
};
