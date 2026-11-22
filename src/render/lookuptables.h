#pragma once

// Colour look-up tables from files, as the engine applies them: .cube (1D
// or 3D, with its domain), or a LUT picture (Hald, tiled squares, or a strip
// across or down). Loaded once per file and kept while it is unchanged.

#include "engine.h"

#include <QString>
#include <memory>

namespace render {
// `tiles`: a square picture is read as tiled squares rather than Hald.
std::shared_ptr<const engine::LookupTable> loadLookupTable(const QString &path, bool tiles, QString *error);
}
