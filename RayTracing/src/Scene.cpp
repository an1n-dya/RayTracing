#include "Scene.h"

#include "EnvironmentSampling.h"

#include <glm/gtc/matrix_transform.hpp>

#include <atomic>
#include <cfloat>

glm::vec3 SkySettings::GetRadiance(const glm::vec3& direction) const {
	switch (Mode) {
	case SkyMode::Gradient:
		return glm::mix(BottomColor, TopColor, 0.5f * (direction.y + 1.0f)) * Intensity;
	case SkyMode::EnvironmentMap:
		if (!Environment)
			return glm::vec3(0.0f);
		return EnvironmentSampling::Lookup(*Environment, direction, EnvironmentRotation / 360.0f) * Intensity;
	default:
		return glm::vec3(0.0f);
	}
}

glm::mat4 ObjectTransform::GetMatrix() const {
	glm::mat4 matrix = glm::translate(glm::mat4(1.0f), Translation);
	matrix = glm::rotate(matrix, glm::radians(Rotation.z), glm::vec3(0, 0, 1));
	matrix = glm::rotate(matrix, glm::radians(Rotation.y), glm::vec3(0, 1, 0));
	matrix = glm::rotate(matrix, glm::radians(Rotation.x), glm::vec3(1, 0, 0));
	return glm::scale(matrix, Scale);
}

void Scene::MarkGeometryChanged() {
	static std::atomic<uint64_t> s_NextVersion{ 1 };
	GeometryVersion = s_NextVersion++;
}

bool Scene::IsValid(const ObjectRef& object) const {
	switch (object.Type) {
	case ObjectType::Sphere:       return object.Index >= 0 && object.Index < (int)Spheres.size();
	case ObjectType::Plane:        return object.Index >= 0 && object.Index < (int)Planes.size();
	case ObjectType::MeshInstance: return object.Index >= 0 && object.Index < (int)MeshInstances.size();
	default:                       return false;
	}
}

int Scene::GetMaterialIndex(const ObjectRef& object) const {
	switch (object.Type) {
	case ObjectType::Sphere:       return Spheres[object.Index].MaterialIndex;
	case ObjectType::Plane:        return Planes[object.Index].MaterialIndex;
	case ObjectType::MeshInstance: return MeshInstances[object.Index].MaterialIndex;
	default:                       return 0;
	}
}

bool Scene::GetBounds(const ObjectRef& object, glm::vec3& min, glm::vec3& max) const {
	if (!IsValid(object))
		return false;

	switch (object.Type) {
	case ObjectType::Sphere: {
		const Sphere& sphere = Spheres[object.Index];
		min = sphere.Position - glm::vec3(sphere.Radius);
		max = sphere.Position + glm::vec3(sphere.Radius);
		return true;
	}
	case ObjectType::MeshInstance: {
		const MeshInstance& instance = MeshInstances[object.Index];
		const Mesh& mesh = Meshes[instance.MeshIndex];
		glm::mat4 transform = instance.Transform.GetMatrix();
		min = glm::vec3(FLT_MAX);
		max = glm::vec3(-FLT_MAX);
		for (int corner = 0; corner < 8; corner++) {
			glm::vec3 local = { corner & 1 ? mesh.BoundsMax.x : mesh.BoundsMin.x, corner & 2 ? mesh.BoundsMax.y : mesh.BoundsMin.y,
				corner & 4 ? mesh.BoundsMax.z : mesh.BoundsMin.z };
			glm::vec3 world = glm::vec3(transform * glm::vec4(local, 1.0f));
			min = glm::min(min, world);
			max = glm::max(max, world);
		}
		return true;
	}
	default:
		return false; // planes are unbounded
	}
}

ObjectRef Scene::AddSphere() {
	Sphere& sphere = Spheres.emplace_back();
	sphere.Radius = 0.5f;
	return { ObjectType::Sphere, (int)Spheres.size() - 1 };
}

ObjectRef Scene::AddPlane() {
	Planes.emplace_back();
	return { ObjectType::Plane, (int)Planes.size() - 1 };
}

ObjectRef Scene::AddMeshInstance(int meshIndex) {
	MeshInstance& instance = MeshInstances.emplace_back();
	instance.MeshIndex = meshIndex;
	return { ObjectType::MeshInstance, (int)MeshInstances.size() - 1 };
}

ObjectRef Scene::Duplicate(const ObjectRef& object) {
	if (!IsValid(object))
		return {};

	switch (object.Type) {
	case ObjectType::Sphere: {
		Sphere copy = Spheres[object.Index];
		copy.Position.x += copy.Radius * 2.5f;
		Spheres.push_back(copy);
		return { ObjectType::Sphere, (int)Spheres.size() - 1 };
	}
	case ObjectType::Plane: {
		Plane copy = Planes[object.Index];
		copy.Point += copy.Normal;
		Planes.push_back(copy);
		return { ObjectType::Plane, (int)Planes.size() - 1 };
	}
	case ObjectType::MeshInstance: {
		MeshInstance copy = MeshInstances[object.Index];
		glm::vec3 min, max;
		GetBounds(object, min, max);
		copy.Transform.Translation.x += (max.x - min.x) * 1.25f;
		MeshInstances.push_back(copy);
		return { ObjectType::MeshInstance, (int)MeshInstances.size() - 1 };
	}
	default:
		return {};
	}
}

void Scene::Remove(const ObjectRef& object) {
	if (!IsValid(object))
		return;

	switch (object.Type) {
	case ObjectType::Sphere: Spheres.erase(Spheres.begin() + object.Index); break;
	case ObjectType::Plane:  Planes.erase(Planes.begin() + object.Index); break;
	case ObjectType::MeshInstance: {
		int meshIndex = MeshInstances[object.Index].MeshIndex;
		MeshInstances.erase(MeshInstances.begin() + object.Index);

		bool stillUsed = false;
		for (const MeshInstance& instance : MeshInstances)
			stillUsed |= instance.MeshIndex == meshIndex;
		if (!stillUsed) {
			Meshes.erase(Meshes.begin() + meshIndex);
			for (MeshInstance& instance : MeshInstances)
				if (instance.MeshIndex > meshIndex)
					instance.MeshIndex--;
			MarkGeometryChanged();
		}
		break;
	}
	default: break;
	}
}

int Scene::FindMesh(const std::string& source) const {
	for (int i = 0; i < (int)Meshes.size(); i++)
		if (Meshes[i].Source == source)
			return i;
	return -1;
}

int Scene::AddMesh(Mesh mesh) {
	Meshes.push_back(std::move(mesh));
	MarkGeometryChanged();
	return (int)Meshes.size() - 1;
}

int Scene::AddMaterial(const Material& material) {
	Materials.push_back(material);
	return (int)Materials.size() - 1;
}

void Scene::RemoveMaterial(int index) {
	if (index < 0 || index >= (int)Materials.size() || Materials.size() <= 1)
		return;

	Materials.erase(Materials.begin() + index);

	auto remap = [index](int& materialIndex) {
		if (materialIndex == index)
			materialIndex = 0;
		else if (materialIndex > index)
			materialIndex--;
	};
	for (Sphere& sphere : Spheres)
		remap(sphere.MaterialIndex);
	for (Plane& plane : Planes)
		remap(plane.MaterialIndex);
	for (MeshInstance& instance : MeshInstances)
		remap(instance.MaterialIndex);
}
