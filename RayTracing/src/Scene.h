#pragma once

#include "glm/glm.hpp"

#include <string>
#include <vector>

struct Material {
	std::string Name = "Material";

	glm::vec3 Albedo{ 1.0f };
	float Roughness = 1.0f;
	float Metallic = 0.0f;
	glm::vec3 EmissionColor{ 0.0f };
	float EmissionPower = 0.0f;

	glm::vec3 GetEmission() const { return EmissionColor * EmissionPower; }
};

struct Sphere {
	glm::vec3 Position{0.0f};
	float Radius = 0.5f;

	int MaterialIndex = 0;
};

enum class SkyMode : int {
	None = 0,     // black: the scene is lit only by emissive materials
	Gradient = 1, // vertical gradient between BottomColor (straight down) and TopColor (straight up)
};

struct SkySettings {
	SkyMode Mode = SkyMode::None;
	glm::vec3 BottomColor{ 1.0f, 1.0f, 1.0f };
	glm::vec3 TopColor{ 0.5f, 0.7f, 1.0f };
	float Intensity = 1.0f;

	glm::vec3 GetRadiance(const glm::vec3& direction) const {
		if (Mode == SkyMode::None)
			return glm::vec3(0.0f);
		float t = 0.5f * (direction.y + 1.0f);
		return glm::mix(BottomColor, TopColor, t) * Intensity;
	}
};

// Identifies one object in a Scene (e.g. the editor's selection, or what a ray hit)
enum class ObjectType : int {
	None = 0,
	Sphere,
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
	std::vector<Material> Materials;
	SkySettings Sky;

	bool IsValid(const ObjectRef& object) const;

	// Returns the new object; copies of existing objects are offset slightly so they're visible
	ObjectRef AddSphere();
	ObjectRef Duplicate(const ObjectRef& object);
	void Remove(const ObjectRef& object);

	int AddMaterial(const Material& material = Material());
	// Objects using the removed material fall back to material 0; the last material can't be removed
	void RemoveMaterial(int index);
};
