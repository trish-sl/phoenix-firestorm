/**
 * @file fscutoutgltf.h
 * @brief Paper cutout: writes the cutout mesh as a glTF file beside its texture
 *
 * $LicenseInfo:firstyear=2026:license=fsviewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026, Trish
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * The Phoenix Firestorm Project, Inc., 1831 Oakwood Drive, Fairmont, Minnesota 56031-3225 USA
 * http://www.firestormviewer.org
 * $/LicenseInfo$
 */

#ifndef FS_CUTOUTGLTF_H
#define FS_CUTOUTGLTF_H

#include "fscutoutmesh.h"

#include <string>

// Writes <gltf_path> (buffers embedded) referencing <png_filename> beside it.
// Material 0 = name (base colour = png, metallic 0, roughness 0.9, OPAQUE, doubleSided false),
// material 1 = "edge" (white, roughness 1). Primitives in face order front, back, edge.
// Returns false if a face is empty or the file cannot be written.
bool fs_write_cutout_gltf(const std::string& gltf_path, const std::string& png_filename,
                          const FSCutout::Mesh& mesh, const std::string& name);

#endif // FS_CUTOUTGLTF_H
