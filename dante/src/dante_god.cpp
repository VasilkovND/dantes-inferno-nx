// dantes-inferno-nx - the game straight from a Games on Demand package (an Xbox 360 hard drive install).
//
// A GOD package is the disc image cut into 162.56 MB pieces (<name>.data/Data0000, Data0001, ...) with hash tables
// between the blocks, plus a header file <name> (STFS header, content type 0x7000, SVOD volume). The SDK's
// StfsContainerDevice (from Xenia) reads it as a disc with plain file reads, no memory mapping, so the console can use
// it: god/ next to the NRO, copied from the hard drive's Content/0000000000000000/454108CF/00007000/ as it is.
//
// How it is mounted: with a package in god/, the runtime's game: and d: start at god_xex/ next to the NRO, and:
//   - d: and \Device\Cdrom0, which the game reads its bigfiles from, are the package;
//   - the package's default.xex is copied into god_xex/ (when it differs), for the runtime to load it from there. The
//     package of Media ID 49028C6A carries the executable already updated to Title Update 2 (version 0.0.2.1; the disc's
//     own is beside it as default.dll), whose loaded image is byte for byte the image this build was made from (the
//     Europe disc 27C36C6C with its TU2): no update then, and none may lie beside it (patching an updated image fails).
//     A package with a disc-version executable gets game/default.xexp copied beside it;
//   - after the executable is loaded, game: is the package too, as it is the disc on the console.
// Extracted disc files in game/ still work as before when there is no god/.

#include "dantes_inferno_pch.h"

#include "dante_god.h"

#include <rex/filesystem/devices/stfs_container_device.h>
#include <rex/filesystem/vfs.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/xcontent.h>
#include <rex/thread.h>

#include <cstdio>
#include <memory>
#include <stdexcept>
#include <vector>

using rex::X_STATUS;  // the SDK's status macros use this unqualified typedef

namespace dante::god {
namespace {

constexpr const char* kDispositivo = "\\Device\\DanteGod";
constexpr uint32_t kTitleId = 0x454108CF;
bool g_montado = false;

struct Cerrar {
  void operator()(rex::filesystem::File* f) const {
    if (f) f->Destroy();
  }
};

// default.xex of the package, whole.
std::vector<uint8_t> LeerXex(rex::filesystem::Device& paquete) {
  rex::filesystem::Entry* entrada = paquete.ResolvePath("default.xex");
  rex::filesystem::File* bruto = nullptr;
  if (!entrada || entrada->Open(rex::filesystem::FileAccess::kGenericRead, &bruto) != X_STATUS_SUCCESS) {
    throw std::runtime_error("the Games on Demand package has no default.xex");
  }
  std::unique_ptr<rex::filesystem::File, Cerrar> archivo(bruto);
  std::vector<uint8_t> datos(entrada->size());
  size_t leidos = 0;
  if (archivo->ReadSync(datos, 0, &leidos) != X_STATUS_SUCCESS || leidos != datos.size()) {
    throw std::runtime_error("cannot read default.xex from the Games on Demand package");
  }
  return datos;
}

// The build field of the XEX execution info's version (major 4, minor 4, build 16, qfe 8 bits); 0 when absent.
uint32_t VersionCompilacion(const std::vector<uint8_t>& xex) {
  auto u32 = [&](size_t o) -> uint32_t {
    return o + 4 <= xex.size() ? (uint32_t(xex[o]) << 24) | (uint32_t(xex[o + 1]) << 16) | (uint32_t(xex[o + 2]) << 8) | xex[o + 3]
                               : 0;
  };
  const uint32_t cabeceras = u32(20);
  for (uint32_t i = 0; i < cabeceras && i < 64; ++i) {
    if (u32(24 + 8 * i) == 0x00040006) {
      return (u32(u32(28 + 8 * i) + 4) >> 8) & 0xFFFF;
    }
  }
  return 0;
}

bool Igual(const std::filesystem::path& ruta, const std::vector<uint8_t>& datos) {
  std::error_code ec;
  if (!std::filesystem::is_regular_file(ruta, ec) || std::filesystem::file_size(ruta, ec) != datos.size()) return false;
  std::vector<uint8_t> actual(datos.size());
  FILE* f = std::fopen(ruta.string().c_str(), "rb");
  if (!f) return false;
  const bool ok = std::fread(actual.data(), 1, actual.size(), f) == actual.size();
  std::fclose(f);
  return ok && actual == datos;
}

void Enlazar(rex::filesystem::VirtualFileSystem& vfs, const char* alias) {
  vfs.UnregisterSymbolicLink(alias);
  if (!vfs.RegisterSymbolicLink(alias, kDispositivo)) {
    throw std::runtime_error(std::string("cannot map ") + alias + " to the Games on Demand package");
  }
}

}  // namespace

bool Montar(rex::Runtime& runtime, const std::filesystem::path& carpeta_god, const std::filesystem::path& carpeta_xex,
            const std::filesystem::path& carpeta_game) {
  std::error_code ec;
  if (!std::filesystem::is_directory(carpeta_god, ec)) return false;
  auto paquete = std::make_unique<rex::filesystem::StfsContainerDevice>(kDispositivo, carpeta_god);
  if (!paquete->Initialize()) {
    throw std::runtime_error("god/ holds no readable Games on Demand package (the header file and its .data folder "
                             "with every DataNNNN piece are needed)");
  }
  const auto& meta = paquete->header().metadata;
  const uint32_t tipo = uint32_t(meta.content_type.get());
  const uint32_t titulo = meta.execution_info.title_id, medio = meta.execution_info.media_id;
  if (titulo != kTitleId) {
    throw std::runtime_error(fmt::format("god/ holds title {:08X}, not Dante's Inferno ({:08X})", titulo, kTitleId));
  }
  REXLOG_INFO("dante: paquete Games on Demand en {}: tipo {:08X}, titulo {:08X}, Media ID {:08X}", carpeta_god.string(),
              tipo, titulo, medio);

  const std::vector<uint8_t> xex = LeerXex(*paquete);
  const auto ruta_xex = carpeta_xex / "default.xex";
  if (!Igual(ruta_xex, xex)) {
    std::filesystem::create_directories(carpeta_xex, ec);
    FILE* f = std::fopen(ruta_xex.string().c_str(), "wb");
    if (!f || std::fwrite(xex.data(), 1, xex.size(), f) != xex.size()) {
      if (f) std::fclose(f);
      throw std::runtime_error("cannot write " + ruta_xex.string());
    }
    std::fclose(f);
    REXLOG_INFO("dante: default.xex copiado del paquete a {} ({} bytes)", ruta_xex.string(), xex.size());
  }
  // Its version (XEX execution info): build 2 and up is Title Update 2 already.
  const auto xexp = carpeta_xex / "default.xexp";
  if (VersionCompilacion(xex) >= 2) {
    std::filesystem::remove(xexp, ec);
    REXLOG_INFO("dante: el ejecutable del paquete ya trae el Title Update 2 (Media ID {:08X})", medio);
  } else if (std::filesystem::is_regular_file(carpeta_game / "default.xexp", ec)) {
    std::filesystem::copy_file(carpeta_game / "default.xexp", xexp, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) throw std::runtime_error("cannot copy game/default.xexp beside the package's default.xex");
  } else {
    throw std::runtime_error("the package's default.xex is the disc version: put Title Update 2's default.xexp in game/");
  }

  rex::filesystem::VirtualFileSystem& vfs = *runtime.file_system();
  if (!vfs.RegisterDevice(std::move(paquete))) throw std::runtime_error("cannot register the Games on Demand package");
  auto cerrojo = rex::thread::global_critical_region::AcquireDirect();
  Enlazar(vfs, "d:");
  Enlazar(vfs, "\\Device\\Cdrom0");
  g_montado = true;
  return true;
}

void DespuesDeCargar(rex::Runtime& runtime) {
  if (!g_montado) return;
  auto cerrojo = rex::thread::global_critical_region::AcquireDirect();
  Enlazar(*runtime.file_system(), "game:");
  REXLOG_INFO("dante: game: y d: son el paquete Games on Demand");
}

bool Montado() { return g_montado; }

}  // namespace dante::god
