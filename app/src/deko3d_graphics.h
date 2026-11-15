#pragma once
// Guest GPU front end and deko3d presentation for the Sims 3 port.
//
// This keeps the guest's Direct3D layer running: it consumes the PM4 ring, honours every packet the
// game can wait on, and raises vblank and ring interrupts. Draws are not translated yet. Each guest
// Swap presents a deko3d clear, so reaching a frame shows the game loop is alive.
#include <memory>

#include <rex/system/interfaces/graphics.h>

namespace sims3 {
std::unique_ptr<rex::system::IGraphicsSystem> CreateDeko3dGraphicsSystem();
}
