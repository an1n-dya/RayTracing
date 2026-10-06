#include "SceneSerializer.h"

#include "Json.h"

#include <algorithm>

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
		case SkyMode::Gradient: return "gradient";
		default:                return "none";
		}
	}

	static SkyMode SkyModeFromString(const std::string& mode) {
		if (mode == "gradient")
			return SkyMode::Gradient;
		return SkyMode::None;
	}

	bool Save(const std::string& path, const Scene& scene, const Camera& camera, std::string* error) {
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
