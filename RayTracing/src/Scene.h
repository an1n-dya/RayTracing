#pragma once

#include "EnvironmentMap.h"
#include "Mesh.h"

#include "glm/glm.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct Material {
	std::string Name = "Material";

	glm::vec3 Albedo{ 1.0f };
	float Roughness = 1.0f;
	float Metallic = 0.0f;
	glm::vec3 EmissionColor{ 0.0f };
	float EmissionPower = 0.0f;
	float Transmission = 0.0f; // 0 = opaque, 1 = glass (only for the non-metallic part)
	float IOR = 1.5f;          // index of refraction; also sets the dielectric specular F0

	glm::vec3 GetEmission() const { return EmissionColor * EmissionPower; }
};

struct Sphere {
	glm::vec3 Position{0.0f};
	float Radius = 0.5f;

	int MaterialIndex = 0;
};

// Infinite plane through Point, double sided
struct Plane {
	glm::vec3 Point{ 0.0f };
	glm::vec3 Normal{ 0.0f, 1.0f, 0.0f }; // unit length
	int MaterialIndex = 0;
};

struct ObjectTransform {
	glm::vec3 Translation{ 0.0f };
	glm::vec3 Rotation{ 0.0f }; // Euler angles in degrees, applied X, then Y, then Z
	glm::vec3 Scale{ 1.0f };

	glm::mat4 GetMatrix() const;
};

// Places a Mesh (by index into Scene::Meshes) in the scene
struct MeshInstance {
	int MeshIndex = 0;
	ObjectTransform Transform;
	int MaterialIndex = 0;
};

enum class SkyMode : int {
	None = 0,           // black: the scene is lit only by emissive materials
	Gradient = 1,       // vertical gradient between BottomColor (straight down) and TopColor (straight up)
	EnvironmentMap = 2, // HDR environment image, importance sampled by light sampling
};

struct SkySettings {
	SkyMode Mode = SkyMode::None;
	glm::vec3 BottomColor{ 1.0f, 1.0f, 1.0f };
	glm::vec3 TopColor{ 0.5f, 0.7f, 1.0f };
	float Intensity = 1.0f;

	// SkyMode::EnvironmentMap
	std::shared_ptr<EnvironmentMap> Environment; // loaded from Environment->GetSource()
	float EnvironmentRotation = 0.0f;            // degrees around +Y

	bool HasEnvironmentMap() const { return Mode == SkyMode::EnvironmentMap && Environment != nullptr; }
	glm::vec3 GetRadiance(const glm::vec3& direction) const;
};

// Identifies one object in a Scene (e.g. the editor's selection, or what a ray hit)
enum class ObjectType : int {
	None = 0,
	Sphere,
	Plane,
	MeshInstance,
};

struct ObjectRef {
	ObjectType Type = ObjectType::None;
	int Index = -1;

	bool IsValid() const { return Type != ObjectType::None && Index >= 0; }
	bool operator==(const ObjectRef& other) const { return Type == other.Type && Index == other.Index; }
	bool operator!=(const ObjectRef& other) const { return !(*this == other); }
};

struct Scene{
	std::vector<Sphere> Spheres;
	std::vector<Plane> Planes;
	std::vector<Mesh> Meshes;
	std::vector<MeshInstance> MeshInstances;
	std::vector<Material> Materials;
	SkySettings Sky;

	// Changes whenever Meshes (the geometry itself, not instance transforms) changes, so renderers know
	// when to rebuild their copies. Unique across all scenes, so loading a different scene also counts.
	uint64_t GeometryVersion = 0;
	void MarkGeometryChanged();

	bool IsValid(const ObjectRef& object) const;
	int GetMaterialIndex(const ObjectRef& object) const;
	// World-space axis-aligned bounds; false for invalid (or unbounded) objects
	bool GetBounds(const ObjectRef& object, glm::vec3& min, glm::vec3& max) const;

	// Returns the new object; copies of existing objects are offset slightly so they're visible
	ObjectRef AddSphere();
	ObjectRef AddPlane();
	ObjectRef AddMeshInstance(int meshIndex);
	ObjectRef Duplicate(const ObjectRef& object);
	void Remove(const ObjectRef& object); // also drops meshes no instance uses any more

	// Index of an already loaded mesh with this source, or -1
	int FindMesh(const std::string& source) const;
	int AddMesh(Mesh mesh);

	int AddMaterial(const Material& material = Material());
	// Objects using the removed material fall back to material 0; the last material can't be removed
	void RemoveMaterial(int index);
};
