#include "Scene.h"

bool Scene::IsValid(const ObjectRef& object) const {
	switch (object.Type) {
	case ObjectType::Sphere: return object.Index >= 0 && object.Index < (int)Spheres.size();
	default:                 return false;
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
	default:
		return false;
	}
}

ObjectRef Scene::AddSphere() {
	Sphere& sphere = Spheres.emplace_back();
	sphere.Radius = 0.5f;
	return { ObjectType::Sphere, (int)Spheres.size() - 1 };
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
	default:
		return {};
	}
}

void Scene::Remove(const ObjectRef& object) {
	if (!IsValid(object))
		return;

	switch (object.Type) {
	case ObjectType::Sphere: Spheres.erase(Spheres.begin() + object.Index); break;
	default: break;
	}
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
}
