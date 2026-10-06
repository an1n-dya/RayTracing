#pragma once

#include "Scene.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

// Per-frame data derived from a Scene, shared by both render paths: instance matrices and the list of
// lights used for next-event estimation (light sampling).
struct PreparedScene {
	struct Instance {
		glm::mat4 ObjectToWorld;
		glm::mat4 WorldToObject;
		glm::mat3 NormalMatrix; // object -> world for normals
	};

	enum class LightType : uint32_t { Sphere = 0, Triangle = 1 };

	// An emissive sphere, or one triangle of an emissive mesh instance (emissive planes are infinite and can't
	// be sampled; they are still found by BSDF sampling). Emission is one-sided: only the front face emits.
	struct Light {
		LightType Type;
		uint32_t ObjectIndex; // sphere or mesh instance index
		uint32_t Triangle;    // triangle within the instance's mesh (triangle lights)
		glm::vec3 Position0;  // sphere center, or the world-space triangle's vertices
		glm::vec3 Position1;
		glm::vec3 Position2;
		glm::vec3 Normal;     // triangles: world-space front-face normal
		float Radius;         // spheres
		float Area;           // triangles (world space)
		glm::vec3 Emission;   // emitted radiance
		float Probability;    // chance of picking this light, proportional to its power
	};

	std::vector<Instance> Instances; // parallel to Scene::MeshInstances
	std::vector<Light> Lights;
	std::vector<float> LightCDF;            // LightCDF[i] = probability of picking a light <= i
	std::vector<int> SphereLights;          // per sphere: index into Lights, or -1
	std::vector<int> InstanceLightOffsets;  // per mesh instance: Lights index of its first triangle, or -1

	// The environment map, when the sky has one worth sampling
	const EnvironmentMap* Environment = nullptr;
	float EnvironmentRotation = 0.0f;    // turns
	float EnvironmentIntensity = 1.0f;
	float EnvironmentProbability = 0.0f; // chance light sampling picks the environment rather than Lights

	bool HasLights() const { return !Lights.empty() || EnvironmentProbability > 0.0f; }

	void Prepare(const Scene& scene);

	// Light picked by a uniform random number u (Lights must not be empty)
	uint32_t SelectLight(float u) const;
};
