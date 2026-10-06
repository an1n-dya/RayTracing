#include "PreparedScene.h"

#include "BSDF.h"

#include <glm/gtc/constants.hpp>

void PreparedScene::Prepare(const Scene& scene) {
	Instances.resize(scene.MeshInstances.size());
	for (size_t i = 0; i < scene.MeshInstances.size(); i++) {
		Instance& instance = Instances[i];
		instance.ObjectToWorld = scene.MeshInstances[i].Transform.GetMatrix();
		instance.WorldToObject = glm::inverse(instance.ObjectToWorld);
		instance.NormalMatrix = glm::transpose(glm::mat3(instance.WorldToObject));
	}

	Lights.clear();
	SphereLights.assign(scene.Spheres.size(), -1);
	InstanceLightOffsets.assign(scene.MeshInstances.size(), -1);
	std::vector<float> power;

	for (size_t i = 0; i < scene.Spheres.size(); i++) {
		const Sphere& sphere = scene.Spheres[i];
		glm::vec3 emission = scene.Materials[sphere.MaterialIndex].GetEmission();
		if (BSDF::Luminance(emission) <= 0.0f)
			continue;

		Light light{};
		light.Type = LightType::Sphere;
		light.ObjectIndex = (uint32_t)i;
		light.Position0 = sphere.Position;
		light.Radius = sphere.Radius;
		light.Emission = emission;
		SphereLights[i] = (int)Lights.size();
		Lights.push_back(light);
		// What a distant viewer sees: a disc of radius r
		power.push_back(BSDF::Luminance(emission) * glm::pi<float>() * sphere.Radius * sphere.Radius);
	}

	for (size_t i = 0; i < scene.MeshInstances.size(); i++) {
		const MeshInstance& meshInstance = scene.MeshInstances[i];
		glm::vec3 emission = scene.Materials[meshInstance.MaterialIndex].GetEmission();
		if (BSDF::Luminance(emission) <= 0.0f)
			continue;

		const Mesh& mesh = scene.Meshes[meshInstance.MeshIndex];
		const glm::mat4& transform = Instances[i].ObjectToWorld;
		InstanceLightOffsets[i] = (int)Lights.size();
		for (uint32_t triangle = 0; triangle < mesh.GetTriangleCount(); triangle++) {
			Light light{};
			light.Type = LightType::Triangle;
			light.ObjectIndex = (uint32_t)i;
			light.Triangle = triangle;
			light.Position0 = glm::vec3(transform * glm::vec4(mesh.Vertices[mesh.Indices[triangle * 3 + 0]].Position, 1.0f));
			light.Position1 = glm::vec3(transform * glm::vec4(mesh.Vertices[mesh.Indices[triangle * 3 + 1]].Position, 1.0f));
			light.Position2 = glm::vec3(transform * glm::vec4(mesh.Vertices[mesh.Indices[triangle * 3 + 2]].Position, 1.0f));
			glm::vec3 cross = glm::cross(light.Position1 - light.Position0, light.Position2 - light.Position0);
			light.Area = 0.5f * glm::length(cross);
			light.Normal = light.Area > 0.0f ? glm::normalize(cross) : glm::vec3(0.0f, 1.0f, 0.0f);
			// The front is the side the authored normals point to (as in Renderer::ClosestHit)
			glm::vec3 vertexNormals = mesh.Vertices[mesh.Indices[triangle * 3 + 0]].Normal + mesh.Vertices[mesh.Indices[triangle * 3 + 1]].Normal
				+ mesh.Vertices[mesh.Indices[triangle * 3 + 2]].Normal;
			if (glm::dot(Instances[i].NormalMatrix * vertexNormals, light.Normal) < 0.0f)
				light.Normal = -light.Normal;
			light.Emission = emission;
			Lights.push_back(light);
			power.push_back(BSDF::Luminance(emission) * light.Area);
		}
	}

	// Pick lights in proportion to their power
	LightCDF.resize(Lights.size());
	float totalPower = 0.0f;
	for (float p : power)
		totalPower += p;
	float cumulative = 0.0f;
	for (size_t i = 0; i < Lights.size(); i++) {
		Lights[i].Probability = totalPower > 0.0f ? power[i] / totalPower : 1.0f / (float)Lights.size();
		cumulative += Lights[i].Probability;
		LightCDF[i] = cumulative;
	}
	if (!LightCDF.empty())
		LightCDF.back() = 1.0f; // guard against rounding leaving the last entry just below 1
}

uint32_t PreparedScene::SelectLight(float u) const {
	// First light whose cumulative probability exceeds u
	uint32_t low = 0, high = (uint32_t)LightCDF.size() - 1;
	while (low < high) {
		uint32_t middle = (low + high) / 2;
		if (u < LightCDF[middle])
			high = middle;
		else
			low = middle + 1;
	}
	return low;
}
