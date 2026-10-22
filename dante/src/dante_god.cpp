// dantes-inferno-nx - the game straight from a Games on Demand package (an Xbox 360 hard drive install).
//
// A GOD package is the disc image cut into 162.56 MB pieces (<name>.data/Data0000, Data0001, ...) with hash tables
// between the blocks, plus a header file <name> (STFS header, content type 0x7000, SVOD volume). The SDK's
// StfsContainerDevice (from Xenia) reads it as a disc with plain file reads, no memory mapping, so the console can use
// it: god/ next to the NRO, copied from the hard drive's Content/0000000000000000/454108CF/00007000/ as it is.
//
// How it is mounted: the runtime maps game: and d: to game/ (the folder with default.xex). With a package in god/:
//   - d: and \Device\Cdrom0, which the game reads its bigfiles from, are the package;
//   - default.xex is copied once from the package into game/ (the runtime loads it from there and applies Title Update
//     2's default.xexp beside it: an update cannot go inside the package);
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

bool Montar(rex::Runtime& runtime, const std::filesystem::path& carpeta_god, const std::filesystem::path& carpeta_game) {
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
  const auto ruta_xex = carpeta_game / "default.xex";
  if (!Igual(ruta_xex, xex)) {
    std::filesystem::create_directories(carpeta_game, ec);
    FILE* f = std::fopen(ruta_xex.string().c_str(), "wb");
    if (!f || std::fwrite(xex.data(), 1, xex.size(), f) != xex.size()) {
      if (f) std::fclose(f);
      throw std::runtime_error("cannot write " + ruta_xex.string());
    }
    std::fclose(f);
    REXLOG_INFO("dante: default.xex copiado del paquete a {} ({} bytes)", ruta_xex.string(), xex.size());
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
