#pragma once

#include "Camera.h"
#include "Scene.h"

#include <string>

// Reads/writes scenes (plus the camera's view and lens) as JSON. See scenes/*.json for examples.
namespace SceneSerializer {

	bool Save(const std::string& path, const Scene& scene, const Camera& camera, std::string* error = nullptr);

	// Replaces scene and the camera's settings with the file's contents; leaves both untouched on failure
	bool Load(const std::string& path, Scene& scene, Camera& camera, std::string* error = nullptr);

}
