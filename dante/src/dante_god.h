// dantes-inferno-nx - the game straight from a Games on Demand package (dante_god.cpp).

#pragma once

#include <filesystem>

namespace rex {
class Runtime;
}

namespace dante::god {

// Before the executable is loaded: if `carpeta_god` holds the game's Games on Demand package, mounts it as d:, copies
// its default.xex into `carpeta_game` when that one differs, and returns true. Throws with a message for the player
// when the folder is there but unusable.
bool Montar(rex::Runtime& runtime, const std::filesystem::path& carpeta_god, const std::filesystem::path& carpeta_game);

// After the executable is loaded: game: becomes the package too.
void DespuesDeCargar(rex::Runtime& runtime);

bool Montado();

}  // namespace dante::god
