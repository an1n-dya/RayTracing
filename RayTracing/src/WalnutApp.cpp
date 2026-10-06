#include "Walnut/Application.h"
#include "Walnut/EntryPoint.h"

#include "Walnut/Image.h"
#include "Walnut/Timer.h"

#include "Renderer.h"
#include "Camera.h"
#include "FileDialogs.h"

#include <glm/gtc/type_ptr.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace Walnut;

// Command-line options. With --render the app renders until --samples samples per pixel have accumulated,
// writes the image (.png = tone mapped, .hdr = linear) and exits.
struct AppOptions {
	std::string RenderOutputPath;
	int RenderSamples = 256;
	int UseGPU = -1; // -1 = keep the default, 0 = --cpu, 1 = --gpu
};

static AppOptions ParseCommandLine(int argc, char** argv) {
	AppOptions options;
	for (int i = 1; i < argc; i++) {
		const char* arg = argv[i];
		bool hasValue = i + 1 < argc;
		if (strcmp(arg, "--render") == 0 && hasValue)
			options.RenderOutputPath = argv[++i];
		else if (strcmp(arg, "--samples") == 0 && hasValue)
			options.RenderSamples = std::max(atoi(argv[++i]), 1);
		else if (strcmp(arg, "--gpu") == 0)
			options.UseGPU = 1;
		else if (strcmp(arg, "--cpu") == 0)
			options.UseGPU = 0;
		else
			fprintf(stderr, "Unknown or incomplete argument: %s\n", arg);
	}
	return options;
}

class AppLayer : public Walnut::Layer {
public:
	AppLayer(const AppOptions& options)
		: m_Camera(45.0f, 0.1f, 100.0f), m_Options(options)
	{
		if (m_Options.UseGPU >= 0)
			m_Renderer.GetSettings().UseGPU = m_Options.UseGPU == 1;
		if (!m_Options.RenderOutputPath.empty())
			m_Renderer.GetSettings().MaxSamples = m_Options.RenderSamples;

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
		if (m_Renderer.GetSettings().MaxSamples > 0)
			ImGui::Text("Samples: %u / %d%s", m_Renderer.GetSampleCount(), m_Renderer.GetSettings().MaxSamples,
				m_Renderer.IsConverged() ? " (done)" : "");
		else
			ImGui::Text("Samples: %u", m_Renderer.GetSampleCount());
		if (ImGui::Button("Render"))
			Render();
		ImGui::SameLine();
		if (ImGui::Button("Export Image..."))
			ExportImage();

		ImGui::Checkbox("Accumulate", &m_Renderer.GetSettings().Accumulate);
		ImGui::SameLine();
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
		ImGui::DragInt("Max Samples", &m_Renderer.GetSettings().MaxSamples, 1.0f, 0, 1 << 20,
			m_Renderer.GetSettings().MaxSamples > 0 ? "%d" : "unlimited");
		// The CPU and GPU paths keep separate accumulation buffers, so restart when switching
		if (ImGui::Checkbox("Use GPU", &m_Renderer.GetSettings().UseGPU))
			m_Renderer.ResetFrameIndex();

		ImGui::BeginDisabled(m_Renderer.GetSettings().UseGPU);
		ImGui::Checkbox("Slow Random", &m_Renderer.GetSettings().SlowRandom);
		ImGui::EndDisabled();

		if (ImGui::Button("Reset"))
			m_Renderer.ResetFrameIndex();

		if (ImGui::CollapsingHeader("Path Tracing", ImGuiTreeNodeFlags_DefaultOpen)) {
			Renderer::Settings& settings = m_Renderer.GetSettings();
			bool changed = false;
			changed |= ImGui::SliderInt("Samples / Frame", &settings.SamplesPerFrame, 1, 64);
			changed |= ImGui::Checkbox("Anti-aliasing", &settings.AntiAliasing);
			changed |= ImGui::SliderInt("Max Bounces", &settings.MaxBounces, 1, 64);
			changed |= ImGui::Checkbox("Russian Roulette", &settings.RussianRoulette);
			if (settings.RussianRoulette)
				changed |= ImGui::SliderInt("RR Start Bounce", &settings.RussianRouletteStartBounce, 0, 16);
			if (changed)
				m_Renderer.ResetFrameIndex();
		}

		if (ImGui::CollapsingHeader("Post-processing", ImGuiTreeNodeFlags_DefaultOpen)) {
			Renderer::Settings& settings = m_Renderer.GetSettings();
			ImGui::SliderFloat("Exposure", &settings.Exposure, -8.0f, 8.0f, "%.2f EV");
			const char* toneMappers[] = { "None (clamp)", "Reinhard", "ACES" };
			int toneMapper = (int)settings.ToneMapping;
			if (ImGui::Combo("Tone Mapping", &toneMapper, toneMappers, IM_ARRAYSIZE(toneMappers)))
				settings.ToneMapping = (ToneMapper)toneMapper;
			ImGui::Checkbox("sRGB Output", &settings.SRGBOutput);
		}

		if (ImGui::CollapsingHeader("Camera", ImGuiTreeNodeFlags_DefaultOpen)) {
			float verticalFOV = m_Camera.GetVerticalFOV();
			if (ImGui::SliderFloat("Vertical FOV", &verticalFOV, 10.0f, 120.0f, "%.1f deg")) {
				m_Camera.SetVerticalFOV(verticalFOV);
				m_Renderer.ResetFrameIndex();
			}
			float moveSpeed = m_Camera.GetMoveSpeed();
			if (ImGui::DragFloat("Move Speed", &moveSpeed, 0.1f, 0.1f, 100.0f, "%.1f units/s"))
				m_Camera.SetMoveSpeed(moveSpeed);
		}

		ImGui::End();

		bool sceneChanged = false;

		ImGui::Begin("Scene");
		for (size_t i = 0; i < m_Scene.Spheres.size(); i++) {
			ImGui::PushID(i);

			Sphere& sphere = m_Scene.Spheres[i];
			sceneChanged |= ImGui::DragFloat3("Position", glm::value_ptr(sphere.Position), 0.1f);
			sceneChanged |= ImGui::DragFloat("Radius", &sphere.Radius, 0.1f);
			sceneChanged |= ImGui::DragInt("Material", &sphere.MaterialIndex, 1.0f, 0, (int)m_Scene.Materials.size() - 1);

			ImGui::Separator();

			ImGui::PopID();
		}

		for (size_t i = 0; i < m_Scene.Materials.size(); i++) {
			ImGui::PushID(i);		

			Material& material = m_Scene.Materials[i];
			sceneChanged |= ImGui::ColorEdit3("Albedo", glm::value_ptr(material.Albedo));
			sceneChanged |= ImGui::DragFloat("Roughness", &material.Roughness, 0.05f, 0.0f, 1.0f);
			sceneChanged |= ImGui::DragFloat("Metallic", &material.Metallic, 0.05f, 0.0f, 1.0f);
			sceneChanged |= ImGui::ColorEdit3("Emission Color", glm::value_ptr(material.EmissionColor));
			sceneChanged |= ImGui::DragFloat("Emission Power", &material.EmissionPower, 0.05f, 0.0f, FLT_MAX);

			ImGui::Separator();

			ImGui::PopID();
		}

		ImGui::End();

		ImGui::Begin("Environment");
		{
			SkySettings& sky = m_Scene.Sky;
			const char* skyModes[] = { "None", "Gradient" };
			int skyMode = (int)sky.Mode;
			if (ImGui::Combo("Sky", &skyMode, skyModes, IM_ARRAYSIZE(skyModes))) {
				sky.Mode = (SkyMode)skyMode;
				sceneChanged = true;
			}
			if (sky.Mode == SkyMode::Gradient) {
				sceneChanged |= ImGui::ColorEdit3("Top Color", glm::value_ptr(sky.TopColor));
				sceneChanged |= ImGui::ColorEdit3("Bottom Color", glm::value_ptr(sky.BottomColor));
			}
			if (sky.Mode != SkyMode::None)
				sceneChanged |= ImGui::DragFloat("Intensity", &sky.Intensity, 0.01f, 0.0f, 100.0f);
		}
		ImGui::End();

		// Any edit invalidates the samples accumulated so far
		if (sceneChanged)
			m_Renderer.ResetFrameIndex();

		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
		ImGui::Begin("Viewport");

		m_ViewportWidth = ImGui::GetContentRegionAvail().x;
		m_ViewportHeight = ImGui::GetContentRegionAvail().y;

		VkDescriptorSet imageDescriptor = m_Renderer.GetFinalImageDescriptorSet();
		if (imageDescriptor)
			ImGui::Image(imageDescriptor,
				{ (float)m_Renderer.GetFinalImageWidth(), (float)m_Renderer.GetFinalImageHeight() },
				ImVec2(0, 1), ImVec2(1, 0));

		ImGui::End();
		ImGui::PopStyleVar();

		Render();

		// Batch mode (--render): save once the requested number of samples has accumulated, then quit
		if (!m_Options.RenderOutputPath.empty() && m_Renderer.IsConverged()) {
			bool saved = m_Renderer.SaveImage(m_Options.RenderOutputPath);
			printf("%s %s (%u spp, %ux%u, %s)\n", saved ? "Saved" : "FAILED to save", m_Options.RenderOutputPath.c_str(),
				m_Renderer.GetSampleCount(), m_Renderer.GetFinalImageWidth(), m_Renderer.GetFinalImageHeight(),
				m_Renderer.GetSettings().UseGPU ? "GPU" : "CPU");
			m_Options.RenderOutputPath.clear();
			Application::Get().Close();
		}
	}

	void OnMenuBar() {
		if (ImGui::BeginMenu("File")) {
			if (ImGui::MenuItem("Export Image..."))
				ExportImage();
			ImGui::Separator();
			if (ImGui::MenuItem("Exit"))
				Application::Get().Close();
			ImGui::EndMenu();
		}
	}

	void ExportImage() {
		std::string path = FileDialogs::SaveFile("PNG image (*.png)\0*.png\0Radiance HDR, linear (*.hdr)\0*.hdr\0", "png");
		if (!path.empty() && !m_Renderer.SaveImage(path))
			fprintf(stderr, "Failed to save image to %s\n", path.c_str());
	}

	void Render() {
		Timer timer;

		m_Renderer.OnResize(m_ViewportWidth, m_ViewportHeight);
		m_Camera.OnResize(m_ViewportWidth, m_ViewportHeight);
		if (!m_Renderer.Render(m_Scene, m_Camera))
			return; // converged: keep showing the last frame's timings

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

	AppOptions m_Options;
};

Walnut::Application* Walnut::CreateApplication(int argc, char** argv) {
	Walnut::ApplicationSpecification spec;
	spec.Name = "Ray Tracing";

	Walnut::Application* app = new Walnut::Application(spec);
	std::shared_ptr<AppLayer> layer = std::make_shared<AppLayer>(ParseCommandLine(argc, argv));
	app->PushLayer(layer);
	app->SetMenubarCallback([layer]() { layer->OnMenuBar(); });
	return app;
}
