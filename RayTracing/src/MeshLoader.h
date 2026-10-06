#pragma once

#include "Mesh.h"

#include <string>

// Loads a mesh from a source string: "builtin:<name>" (see BuiltinMeshes) or a file path.
// Supported files: Wavefront .obj (positions, normals, polygonal faces; materials/UVs are ignored).
// Other formats can be added here, e.g. by plugging in a library such as tinyobjloader or cgltf.
namespace MeshLoader {

	bool Load(const std::string& source, Mesh& mesh, std::string* error = nullptr);

}
