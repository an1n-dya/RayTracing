#include "Walnut/Application.h"
#include "Walnut/EntryPoint.h"

#include "Walnut/Image.h"
#include "Walnut/Timer.h"

#include "Renderer.h"
#include "Camera.h"

#include <glm/gtc/type_ptr.hpp>	

using namespace Walnut;

class AppLayer : public Walnut::Layer {
public:
	AppLayer()
		: m_Camera(45.0f, 0.1f, 100.0f)
	{
		Material& pinkSphere = m_Scene.Materials.emplace_back();
		pinkSphere.Albedo = { 1.0f, 0.0f, 1.0f };
		pinkSphere.Roughness = 0.0f;

		Material& blueSphere = m_Scene.Materials.emplace_back();
		blueSphere.Albedo = { 0.2f, 0.3f, 1.0f };
		blueSphere.Roughness = 0.1f;

		Material& orangeSphere = m_Scene.Materials.emplace_back();
		orangeSphere.Albedo = { 0.8f, 0.5f, 0.2f };
		orangeSphere.Roughness = 0.1f;
		orangeSphere.EmissionColor = orangeSphere.Albedo;
		orangeSphere.EmissionPower = 2.0f;

		{
			Sphere sphere;
			sphere.Position = { 0.0f, 0.0f, 0.0f };
			sphere.Radius = 1.0f;
			sphere.MaterialIndex = 0;
			m_Scene.Spheres.push_back(sphere);
		}

		{
			Sphere sphere;
			sphere.Position = { 0.0f, -101.0f, 0.0f };
			sphere.Radius = 100.0f;
			sphere.MaterialIndex = 1;
			m_Scene.Spheres.push_back(sphere);
		}

		{
			Sphere sphere;
			sphere.Position = { 2.0f, 0.0f, 0.0f };
			sphere.Radius = 1.0f;
			sphere.MaterialIndex = 2;
			m_Scene.Spheres.push_back(sphere);
		}
	}

	virtual void OnUpdate(float ts) override {
		if (m_Camera.OnUpdate(ts))
			m_Renderer.ResetFrameIndex();
	}

	virtual void OnUIRender() override {
		ImGui::Begin("Settings");
		ImGui::Text("Last render: %.3fms (%.1f FPS)", m_LastRenderTime, m_FPS);
		if (ImGui::Button("Render"))
			Render();

		ImGui::Checkbox("Accumulate", &m_Renderer.GetSettings().Accumulate);
		ImGui::Checkbox("Slow Random", &m_Renderer.GetSettings().SlowRandom);
		ImGui::Checkbox("Sky Box", &m_Renderer.GetSettings().SkyBox);

		// BVH Controls
		ImGui::Separator();
		ImGui::Text("BVH Acceleration");
		bool useBVH = m_Renderer.GetSettings().UseBVH;
		if (ImGui::Checkbox("Use BVH", &useBVH)) {
			m_Renderer.GetSettings().UseBVH = useBVH;
			m_Renderer.ResetFrameIndex();
		}

		// BVH Statistics
		if (m_Renderer.GetSettings().UseBVH) {
			const BVH& bvh = m_Renderer.GetBVH();
			ImGui::Text("BVH Stats:");
			ImGui::Text("  Nodes: %u", bvh.GetNodeCount());
			ImGui::Text("  Leaves: %u", bvh.GetLeafCount());
			ImGui::Text("  Max Depth: %u", bvh.GetMaxDepth());
		}

		if (ImGui::Button("Reset"))
			m_Renderer.ResetFrameIndex();

		ImGui::End();

		ImGui::Begin("Scene");

		// Button to add more spheres for testing BVH performance
		if (ImGui::Button("Add Random Sphere")) {
			Sphere sphere;
			sphere.Position = {
				(rand() / (float)RAND_MAX - 0.5f) * 20.0f,
				(rand() / (float)RAND_MAX - 0.5f) * 10.0f,
				(rand() / (float)RAND_MAX - 0.5f) * 20.0f
			};
			sphere.Radius = 0.5f + (rand() / (float)RAND_MAX) * 1.5f;
			sphere.MaterialIndex = rand() % m_Scene.Materials.size();
			m_Scene.Spheres.push_back(sphere);
			m_SceneModified = true;
		}

		ImGui::SameLine();
		if (ImGui::Button("Clear Extra Spheres")) {
			if (m_Scene.Spheres.size() > 3) {
				m_Scene.Spheres.resize(3); // Keep original 3 spheres
				m_SceneModified = true;
			}
		}

		ImGui::Text("Total Spheres: %zu", m_Scene.Spheres.size());

		ImGui::Separator();

		for (size_t i = 0; i < m_Scene.Spheres.size(); i++) {
			ImGui::PushID(i);

			Sphere& sphere = m_Scene.Spheres[i];
			bool changed = false;
			changed |= ImGui::DragFloat3("Position", glm::value_ptr(sphere.Position), 0.1f);
			changed |= ImGui::DragFloat("Radius", &sphere.Radius, 0.1f);
			changed |= ImGui::DragInt("Material", &sphere.MaterialIndex, 1.0f, 0, (int)m_Scene.Materials.size() - 1);

			if (changed) {
				m_SceneModified = true;
			}

			ImGui::Separator();

			ImGui::PopID();
		}

		for (size_t i = 0; i < m_Scene.Materials.size(); i++) {
			ImGui::PushID(i);

			Material& material = m_Scene.Materials[i];
			ImGui::ColorEdit3("Albedo", glm::value_ptr(material.Albedo));
			ImGui::DragFloat("Roughness", &material.Roughness, 0.05f, 0.0f, 1.0f);
			ImGui::DragFloat("Metallic", &material.Metallic, 0.05f, 0.0f, 1.0f);
			ImGui::ColorEdit3("Emission Color", glm::value_ptr(material.EmissionColor));
			ImGui::DragFloat("Emission Power", &material.EmissionPower, 0.05f, 0.0f, FLT_MAX);

			ImGui::Separator();

			ImGui::PopID();
		}

		ImGui::End();

		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
		ImGui::Begin("Viewport");

		m_ViewportWidth = ImGui::GetContentRegionAvail().x;
		m_ViewportHeight = ImGui::GetContentRegionAvail().y;

		auto image = m_Renderer.GetFinalImage();
		if (image)
			ImGui::Image(image->GetDescriptorSet(), { (float)image->GetWidth(), (float)image->GetHeight() },
				ImVec2(0, 1), ImVec2(1, 0));

		ImGui::End();
		ImGui::PopStyleVar();

		Render();
	}

	void Render() {
		Timer timer;

		m_Renderer.OnResize(m_ViewportWidth, m_ViewportHeight);
		m_Camera.OnResize(m_ViewportWidth, m_ViewportHeight);

		// Mark BVH for rebuild if scene was modified
		if (m_SceneModified) {
			// The renderer will detect this and rebuild BVH automatically
			m_Renderer.ResetFrameIndex();
			m_SceneModified = false;
		}

		m_Renderer.Render(m_Scene, m_Camera);

		m_LastRenderTime = timer.ElapsedMillis();
		m_FPS = m_LastRenderTime > 0.0f ? 1000.0f / m_LastRenderTime : 0.0f;
	}

private:
	Renderer m_Renderer;
	Camera m_Camera;
	Scene m_Scene;
	uint32_t m_ViewportWidth = 0, m_ViewportHeight = 0;

	float m_LastRenderTime = 0.0f;
	float m_FPS = 0.0f;

	bool m_SceneModified = false;
};

Walnut::Application* Walnut::CreateApplication(int argc, char** argv) {
	Walnut::ApplicationSpecification spec;
	spec.Name = "Ray Tracing";

	Walnut::Application* app = new Walnut::Application(spec);
	app->PushLayer<AppLayer>();
	app->SetMenubarCallback([app]()
		{
			if (ImGui::BeginMenu("File"))
			{
				if (ImGui::MenuItem("Exit"))
				{
					app->Close();
				}
				ImGui::EndMenu();
			}
		});
	return app;
}
