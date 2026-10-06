#include "SceneSerializer.h"

#include "Json.h"
#include "MeshLoader.h"

#include <algorithm>
#include <filesystem>

namespace SceneSerializer {

	static constexpr int FormatVersion = 1;

	static Json::Value ToJson(const glm::vec3& v) {
		Json::Value array = Json::Value::MakeArray();
		array.Append(v.x);
		array.Append(v.y);
		array.Append(v.z);
		return array;
	}

	static glm::vec3 ReadVec3(const Json::Value& value, const glm::vec3& fallback) {
		if (!value.IsArray() || value.Size() != 3)
			return fallback;
		return { value[0].AsFloat(fallback.x), value[1].AsFloat(fallback.y), value[2].AsFloat(fallback.z) };
	}

	static const char* SkyModeToString(SkyMode mode) {
		switch (mode) {
		case SkyMode::Gradient:       return "gradient";
		case SkyMode::EnvironmentMap: return "environment";
		default:                      return "none";
		}
	}

	static SkyMode SkyModeFromString(const std::string& mode) {
		if (mode == "gradient")
			return SkyMode::Gradient;
		if (mode == "environment")
			return SkyMode::EnvironmentMap;
		return SkyMode::None;
	}

	static bool IsBuiltin(const std::string& source) {
		return source.rfind("builtin:", 0) == 0;
	}

	// Mesh and image files near the scene file (at most two folders up) are stored relative to it, so scene folders
	// can be moved around; anything further away keeps its absolute path
	static std::string MakeRelative(const std::string& source, const std::filesystem::path& sceneDirectory) {
		if (IsBuiltin(source))
			return source;
		std::error_code ec;
		std::filesystem::path relative = std::filesystem::relative(source, sceneDirectory, ec);
		if (ec || relative.empty())
			return source;

		int levelsUp = 0;
		for (const std::filesystem::path& part : relative)
			levelsUp += part == "..";
		if (levelsUp > 2)
			return std::filesystem::absolute(source).generic_string();
		return relative.generic_string();
	}

	static std::string Resolve(const std::string& source, const std::filesystem::path& sceneDirectory) {
		if (IsBuiltin(source) || std::filesystem::path(source).is_absolute())
			return source;
		return (sceneDirectory / source).lexically_normal().string();
	}

	bool Save(const std::string& path, const Scene& scene, const Camera& camera, std::string* error) {
		std::filesystem::path sceneDirectory = std::filesystem::absolute(path).parent_path();

		Json::Value root = Json::Value::MakeObject();
		root["version"] = FormatVersion;

		Json::Value& cameraJson = root["camera"];
		cameraJson["position"] = ToJson(camera.GetPosition());
		cameraJson["direction"] = ToJson(camera.GetDirection());
		cameraJson["verticalFov"] = camera.GetVerticalFOV();
		cameraJson["aperture"] = camera.GetAperture();
		cameraJson["focusDistance"] = camera.GetFocusDistance();
		cameraJson["moveSpeed"] = camera.GetMoveSpeed();

		Json::Value& sky = root["sky"];
		sky["mode"] = SkyModeToString(scene.Sky.Mode);
		sky["topColor"] = ToJson(scene.Sky.TopColor);
		sky["bottomColor"] = ToJson(scene.Sky.BottomColor);
		sky["intensity"] = scene.Sky.Intensity;
		if (scene.Sky.Environment)
			sky["environment"] = MakeRelative(scene.Sky.Environment->GetSource(), sceneDirectory);
		sky["rotation"] = scene.Sky.EnvironmentRotation;

		Json::Value& materials = root["materials"] = Json::Value::MakeArray();
		for (const Material& material : scene.Materials) {
			Json::Value& json = materials.Append(Json::Value::MakeObject());
			json["name"] = material.Name;
			json["albedo"] = ToJson(material.Albedo);
			json["roughness"] = material.Roughness;
			json["metallic"] = material.Metallic;
			json["transmission"] = material.Transmission;
			json["ior"] = material.IOR;
			json["emissionColor"] = ToJson(material.EmissionColor);
			json["emissionPower"] = material.EmissionPower;
		}

		Json::Value& spheres = root["spheres"] = Json::Value::MakeArray();
		for (const Sphere& sphere : scene.Spheres) {
			Json::Value& json = spheres.Append(Json::Value::MakeObject());
			json["position"] = ToJson(sphere.Position);
			json["radius"] = sphere.Radius;
			json["material"] = sphere.MaterialIndex;
		}

		Json::Value& planes = root["planes"] = Json::Value::MakeArray();
		for (const Plane& plane : scene.Planes) {
			Json::Value& json = planes.Append(Json::Value::MakeObject());
			json["point"] = ToJson(plane.Point);
			json["normal"] = ToJson(plane.Normal);
			json["material"] = plane.MaterialIndex;
		}

		Json::Value& meshes = root["meshes"] = Json::Value::MakeArray();
		for (const Mesh& mesh : scene.Meshes) {
			Json::Value& json = meshes.Append(Json::Value::MakeObject());
			json["name"] = mesh.Name;
			json["source"] = MakeRelative(mesh.Source, sceneDirectory);
		}

		Json::Value& instances = root["meshInstances"] = Json::Value::MakeArray();
		for (const MeshInstance& instance : scene.MeshInstances) {
			Json::Value& json = instances.Append(Json::Value::MakeObject());
			json["mesh"] = instance.MeshIndex;
			json["position"] = ToJson(instance.Transform.Translation);
			json["rotation"] = ToJson(instance.Transform.Rotation);
			json["scale"] = ToJson(instance.Transform.Scale);
			json["material"] = instance.MaterialIndex;
		}

		return Json::WriteFile(path, root, error);
	}

	bool Load(const std::string& path, Scene& scene, Camera& camera, std::string* error) {
		Json::Value root;
		if (!Json::ReadFile(path, root, error))
			return false;
		if (!root.IsObject()) {
			if (error)
				*error = path + ": expected a JSON object at the top level";
			return false;
		}
		if (root["version"].AsInt(FormatVersion) > FormatVersion) {
			if (error)
				*error = path + ": written by a newer version of the app (format version " + std::to_string(root["version"].AsInt()) + ")";
			return false;
		}

		// Missing fields keep their defaults, so hand-written files can be short
		Scene loaded;

		const Json::Value& sky = root["sky"];
		loaded.Sky.Mode = SkyModeFromString(sky["mode"].AsString("none"));
		loaded.Sky.TopColor = ReadVec3(sky["topColor"], loaded.Sky.TopColor);
		loaded.Sky.BottomColor = ReadVec3(sky["bottomColor"], loaded.Sky.BottomColor);
		loaded.Sky.Intensity = sky["intensity"].AsFloat(loaded.Sky.Intensity);
		loaded.Sky.EnvironmentRotation = sky["rotation"].AsFloat(loaded.Sky.EnvironmentRotation);

		std::filesystem::path sceneDirectory = std::filesystem::absolute(path).parent_path();
		if (sky["environment"].IsString()) {
			std::string environmentError;
			loaded.Sky.Environment = EnvironmentMap::Load(Resolve(sky["environment"].AsString(), sceneDirectory), &environmentError);
			if (!loaded.Sky.Environment) {
				if (error)
					*error = path + ": " + environmentError;
				return false;
			}
		}

		for (const Json::Value& json : root["materials"].Elements()) {
			Material material;
			material.Name = json["name"].AsString("Material " + std::to_string(loaded.Materials.size()));
			material.Albedo = ReadVec3(json["albedo"], material.Albedo);
			material.Roughness = json["roughness"].AsFloat(material.Roughness);
			material.Metallic = json["metallic"].AsFloat(material.Metallic);
			material.Transmission = json["transmission"].AsFloat(material.Transmission);
			material.IOR = json["ior"].AsFloat(material.IOR);
			material.EmissionColor = ReadVec3(json["emissionColor"], material.EmissionColor);
			material.EmissionPower = json["emissionPower"].AsFloat(material.EmissionPower);
			loaded.Materials.push_back(material);
		}
		if (loaded.Materials.empty()) // objects always need a material to point at
			loaded.Materials.emplace_back().Name = "Default";

		int maxMaterial = (int)loaded.Materials.size() - 1;
		for (const Json::Value& json : root["spheres"].Elements()) {
			Sphere sphere;
			sphere.Position = ReadVec3(json["position"], sphere.Position);
			sphere.Radius = std::max(json["radius"].AsFloat(sphere.Radius), 0.001f);
			sphere.MaterialIndex = std::clamp(json["material"].AsInt(0), 0, maxMaterial);
			loaded.Spheres.push_back(sphere);
		}

		for (const Json::Value& json : root["planes"].Elements()) {
			Plane plane;
			plane.Point = ReadVec3(json["point"], plane.Point);
			glm::vec3 normal = ReadVec3(json["normal"], plane.Normal);
			plane.Normal = glm::dot(normal, normal) > 0.0f ? glm::normalize(normal) : glm::vec3(0, 1, 0);
			plane.MaterialIndex = std::clamp(json["material"].AsInt(0), 0, maxMaterial);
			loaded.Planes.push_back(plane);
		}

		for (const Json::Value& json : root["meshes"].Elements()) {
			Mesh mesh;
			std::string meshError;
			if (!MeshLoader::Load(Resolve(json["source"].AsString(), sceneDirectory), mesh, &meshError)) {
				if (error)
					*error = path + ": " + meshError;
				return false;
			}
			mesh.Name = json["name"].AsString(mesh.Name);
			loaded.Meshes.push_back(std::move(mesh));
		}

		int maxMesh = (int)loaded.Meshes.size() - 1;
		for (const Json::Value& json : root["meshInstances"].Elements()) {
			if (maxMesh < 0)
				break;
			MeshInstance instance;
			instance.MeshIndex = std::clamp(json["mesh"].AsInt(0), 0, maxMesh);
			instance.Transform.Translation = ReadVec3(json["position"], instance.Transform.Translation);
			instance.Transform.Rotation = ReadVec3(json["rotation"], instance.Transform.Rotation);
			instance.Transform.Scale = ReadVec3(json["scale"], instance.Transform.Scale);
			instance.MaterialIndex = std::clamp(json["material"].AsInt(0), 0, maxMaterial);
			loaded.MeshInstances.push_back(instance);
		}
		loaded.MarkGeometryChanged();

		const Json::Value& cameraJson = root["camera"];
		if (cameraJson.IsObject()) {
			camera.SetView(ReadVec3(cameraJson["position"], camera.GetPosition()), ReadVec3(cameraJson["direction"], camera.GetDirection()));
			camera.SetVerticalFOV(cameraJson["verticalFov"].AsFloat(camera.GetVerticalFOV()));
			camera.SetAperture(std::max(cameraJson["aperture"].AsFloat(camera.GetAperture()), 0.0f));
			camera.SetFocusDistance(std::max(cameraJson["focusDistance"].AsFloat(camera.GetFocusDistance()), 0.01f));
			camera.SetMoveSpeed(cameraJson["moveSpeed"].AsFloat(camera.GetMoveSpeed()));
		}

		scene = std::move(loaded);
		return true;
	}

}
