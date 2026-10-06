#include "Walnut/Application.h"
#include "Walnut/EntryPoint.h"

#include "Walnut/Image.h"
#include "Walnut/Timer.h"

#include "Renderer.h"
#include "Camera.h"
#include "FileDialogs.h"

#include "imgui_internal.h" // DockId lookup for default panel placement

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
	CompareMode Compare = CompareMode::Off; // --compare split|difference
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
		else if (strcmp(arg, "--compare") == 0 && hasValue) {
			const char* mode = argv[++i];
			options.Compare = strcmp(mode, "split") == 0 ? CompareMode::Split : strcmp(mode, "difference") == 0 ? CompareMode::Difference : CompareMode::Off;
		}
		else
			fprintf(stderr, "Unknown or incomplete argument: %s\n", arg);
	}
	return options;
}

// Panels without a saved layout (e.g. ones added after imgui.ini was written) open as tabs next to the
// Scene panel instead of floating over the viewport
static void DockNextToScenePanel() {
	ImGuiWindow* scenePanel = ImGui::FindWindowByName("Scene");
	if (scenePanel && scenePanel->DockId != 0)
		ImGui::SetNextWindowDockID(scenePanel->DockId, ImGuiCond_FirstUseEver);
}

class AppLayer : public Walnut::Layer {
public:
	AppLayer(const AppOptions& options)
		: m_Camera(45.0f, 0.1f, 100.0f), m_Options(options)
	{
		if (m_Options.UseGPU >= 0)
			m_Renderer.GetSettings().UseGPU = m_Options.UseGPU == 1;
		m_Renderer.GetSettings().Compare = m_Options.Compare;
		if (!m_Options.RenderOutputPath.empty())
			m_Renderer.GetSettings().MaxSamples = m_Options.RenderSamples;

		CreateDefaultScene();
	}

	virtual void OnUpdate(float ts) override {
		if (m_Camera.OnUpdate(ts))
			m_Renderer.ResetFrameIndex();
	}

	virtual void OnUIRender() override {
		m_SceneChanged = false;

		DrawSettingsPanel();
		DrawScenePanel();
		DrawMaterialsPanel();
		DrawEnvironmentPanel();

		// Any edit invalidates the samples accumulated so far
		if (m_SceneChanged)
			m_Renderer.ResetFrameIndex();

		DrawViewport();

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

private:
	void CreateDefaultScene() {
		Material& pinkSphere = m_Scene.Materials.emplace_back();
		pinkSphere.Name = "Pink";
		pinkSphere.Albedo = { 1.0f, 0.0f, 1.0f };
		pinkSphere.Roughness = 0.0f;

		Material& blueSphere = m_Scene.Materials.emplace_back();
		blueSphere.Name = "Blue";
		blueSphere.Albedo = { 0.2f, 0.3f, 1.0f };
		blueSphere.Roughness = 0.1f;

		Material& orangeSphere = m_Scene.Materials.emplace_back();
		orangeSphere.Name = "Orange Light";
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

	void DrawSettingsPanel() {
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

		{
			Renderer::Settings& settings = m_Renderer.GetSettings();
			const char* compareModes[] = { "Off", "Split view", "Difference" };
			int compareMode = (int)settings.Compare;
			// Both paths need to start from the same (empty) accumulation to be comparable
			if (ImGui::Combo("Compare CPU/GPU", &compareMode, compareModes, IM_ARRAYSIZE(compareModes))) {
				settings.Compare = (CompareMode)compareMode;
				m_Renderer.ResetFrameIndex();
			}
			if (settings.Compare == CompareMode::Split)
				ImGui::TextDisabled("Drag the divider in the viewport (CPU left, GPU right)");
			if (settings.Compare == CompareMode::Difference) {
				ImGui::SliderFloat("Difference Scale", &settings.DifferenceScale, 1.0f, 64.0f, "x%.0f", ImGuiSliderFlags_Logarithmic);
				ImGui::Text("Mean |CPU - GPU|: %.3f / 255", m_Renderer.GetCompareMeanError());
			}
		}

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

			float aperture = m_Camera.GetAperture();
			if (ImGui::DragFloat("Aperture", &aperture, 0.005f, 0.0f, 10.0f, aperture > 0.0f ? "%.3f" : "pinhole")) {
				m_Camera.SetAperture(std::max(aperture, 0.0f));
				m_Renderer.ResetFrameIndex();
			}
			float focusDistance = m_Camera.GetFocusDistance();
			if (ImGui::DragFloat("Focus Distance", &focusDistance, 0.05f, 0.01f, 1000.0f)) {
				m_Camera.SetFocusDistance(std::max(focusDistance, 0.01f));
				m_Renderer.ResetFrameIndex();
			}
			ImGui::BeginDisabled(!m_Selection.IsValid());
			if (ImGui::Button("Focus on Selection")) {
				glm::vec3 min, max;
				if (m_Scene.GetBounds(m_Selection, min, max)) {
					float distance = glm::dot(0.5f * (min + max) - m_Camera.GetPosition(), m_Camera.GetDirection());
					m_Camera.SetFocusDistance(std::max(distance, 0.01f));
					m_Renderer.ResetFrameIndex();
				}
			}
			ImGui::EndDisabled();
			ImGui::SameLine();
			ImGui::TextDisabled("(or Ctrl+click in the viewport)");
		}

		ImGui::End();
	}

	static std::string GetObjectLabel(const Scene& scene, const ObjectRef& object) {
		char label[128];
		switch (object.Type) {
		case ObjectType::Sphere: {
			const Sphere& sphere = scene.Spheres[object.Index];
			snprintf(label, sizeof(label), "Sphere %d (%s)", object.Index, scene.Materials[sphere.MaterialIndex].Name.c_str());
			break;
		}
		default:
			snprintf(label, sizeof(label), "?");
			break;
		}
		return label;
	}

	// Scene panel: the object list plus the selected object's properties
	void DrawScenePanel() {
		ImGui::Begin("Scene");

		if (ImGui::Button("Add Sphere")) {
			m_Selection = m_Scene.AddSphere();
			m_SceneChanged = true;
		}

		if (ImGui::BeginListBox("##Objects", ImVec2(-FLT_MIN, 8 * ImGui::GetTextLineHeightWithSpacing()))) {
			for (int i = 0; i < (int)m_Scene.Spheres.size(); i++) {
				ObjectRef object{ ObjectType::Sphere, i };
				ImGui::PushID(i);
				if (ImGui::Selectable(GetObjectLabel(m_Scene, object).c_str(), m_Selection == object))
					m_Selection = object;
				ImGui::PopID();
			}
			ImGui::EndListBox();
		}

		if (!m_Scene.IsValid(m_Selection))
			m_Selection = {};

		if (m_Selection.IsValid()) {
			ImGui::Separator();
			ImGui::TextUnformatted(GetObjectLabel(m_Scene, m_Selection).c_str());

			if (m_Selection.Type == ObjectType::Sphere) {
				Sphere& sphere = m_Scene.Spheres[m_Selection.Index];
				m_SceneChanged |= ImGui::DragFloat3("Position", glm::value_ptr(sphere.Position), 0.1f);
				if (ImGui::DragFloat("Radius", &sphere.Radius, 0.01f, 0.001f, 10000.0f)) {
					sphere.Radius = std::max(sphere.Radius, 0.001f);
					m_SceneChanged = true;
				}
				m_SceneChanged |= MaterialCombo("Material", sphere.MaterialIndex);
			}

			if (ImGui::Button("Duplicate")) {
				m_Selection = m_Scene.Duplicate(m_Selection);
				m_SceneChanged = true;
			}
			ImGui::SameLine();
			if (ImGui::Button("Delete")) {
				m_Scene.Remove(m_Selection);
				m_Selection = {};
				m_SceneChanged = true;
			}
		}

		ImGui::End();
	}

	bool MaterialCombo(const char* label, int& materialIndex) {
		bool changed = false;
		if (ImGui::BeginCombo(label, m_Scene.Materials[materialIndex].Name.c_str())) {
			for (int i = 0; i < (int)m_Scene.Materials.size(); i++) {
				ImGui::PushID(i);
				if (ImGui::Selectable(m_Scene.Materials[i].Name.c_str(), i == materialIndex)) {
					materialIndex = i;
					changed = true;
				}
				ImGui::PopID();
			}
			ImGui::EndCombo();
		}
		return changed;
	}

	void DrawMaterialsPanel() {
		DockNextToScenePanel();
		ImGui::Begin("Materials");

		if (ImGui::Button("Add Material")) {
			Material material;
			material.Name = "Material " + std::to_string(m_Scene.Materials.size());
			m_Scene.AddMaterial(material);
		}

		int materialToRemove = -1;
		for (int i = 0; i < (int)m_Scene.Materials.size(); i++) {
			ImGui::PushID(i);

			Material& material = m_Scene.Materials[i];
			// "###" keeps the header's ID stable while its name is being edited
			std::string header = material.Name + "###Material";
			if (ImGui::CollapsingHeader(header.c_str())) {
				char name[128];
				snprintf(name, sizeof(name), "%s", material.Name.c_str());
				if (ImGui::InputText("Name", name, sizeof(name)))
					material.Name = name;

				m_SceneChanged |= ImGui::ColorEdit3("Albedo", glm::value_ptr(material.Albedo));
				m_SceneChanged |= ImGui::DragFloat("Roughness", &material.Roughness, 0.01f, 0.0f, 1.0f);
				m_SceneChanged |= ImGui::DragFloat("Metallic", &material.Metallic, 0.01f, 0.0f, 1.0f);
				m_SceneChanged |= ImGui::DragFloat("Transmission", &material.Transmission, 0.01f, 0.0f, 1.0f);
				m_SceneChanged |= ImGui::DragFloat("IOR", &material.IOR, 0.005f, 1.0f, 3.0f);
				m_SceneChanged |= ImGui::ColorEdit3("Emission Color", glm::value_ptr(material.EmissionColor));
				m_SceneChanged |= ImGui::DragFloat("Emission Power", &material.EmissionPower, 0.05f, 0.0f, FLT_MAX);

				ImGui::BeginDisabled(m_Scene.Materials.size() <= 1);
				if (ImGui::Button("Delete Material"))
					materialToRemove = i;
				ImGui::EndDisabled();
			}

			ImGui::PopID();
		}

		if (materialToRemove >= 0) {
			m_Scene.RemoveMaterial(materialToRemove);
			m_SceneChanged = true;
		}

		ImGui::End();
	}

	void DrawEnvironmentPanel() {
		DockNextToScenePanel();
		ImGui::Begin("Environment");

		SkySettings& sky = m_Scene.Sky;
		const char* skyModes[] = { "None", "Gradient" };
		int skyMode = (int)sky.Mode;
		if (ImGui::Combo("Sky", &skyMode, skyModes, IM_ARRAYSIZE(skyModes))) {
			sky.Mode = (SkyMode)skyMode;
			m_SceneChanged = true;
		}
		if (sky.Mode == SkyMode::Gradient) {
			m_SceneChanged |= ImGui::ColorEdit3("Top Color", glm::value_ptr(sky.TopColor));
			m_SceneChanged |= ImGui::ColorEdit3("Bottom Color", glm::value_ptr(sky.BottomColor));
		}
		if (sky.Mode != SkyMode::None)
			m_SceneChanged |= ImGui::DragFloat("Intensity", &sky.Intensity, 0.01f, 0.0f, 100.0f);

		ImGui::End();
	}

	void DrawViewport() {
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
		ImGui::Begin("Viewport");

		m_ViewportWidth = (uint32_t)ImGui::GetContentRegionAvail().x;
		m_ViewportHeight = (uint32_t)ImGui::GetContentRegionAvail().y;

		Renderer::Settings& settings = m_Renderer.GetSettings();
		bool splitView = settings.Compare == CompareMode::Split && m_Renderer.GetGPUImageDescriptorSet();

		VkDescriptorSet imageDescriptor = splitView ? m_Renderer.GetCPUImageDescriptorSet() : m_Renderer.GetFinalImageDescriptorSet();
		if (imageDescriptor) {
			ImVec2 imageSize = { (float)m_Renderer.GetFinalImageWidth(), (float)m_Renderer.GetFinalImageHeight() };
			ImGui::Image(imageDescriptor, imageSize, ImVec2(0, 1), ImVec2(1, 0));
			ImVec2 imageMin = ImGui::GetItemRectMin();
			ImVec2 imageMax = ImGui::GetItemRectMax();
			bool hovered = ImGui::IsItemHovered();
			bool clicked = hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsMouseDown(ImGuiMouseButton_Right);

			if (splitView) {
				// GPU image to the right of the divider, drawn over the CPU image
				float dividerX = imageMin.x + settings.CompareSplit * imageSize.x;
				ImDrawList* drawList = ImGui::GetWindowDrawList();
				drawList->AddImage(m_Renderer.GetGPUImageDescriptorSet(), ImVec2(dividerX, imageMin.y), imageMax,
					ImVec2(settings.CompareSplit, 1.0f), ImVec2(1.0f, 0.0f));
				drawList->AddLine(ImVec2(dividerX, imageMin.y), ImVec2(dividerX, imageMax.y), IM_COL32(255, 255, 255, 200), 2.0f);
				drawList->AddText(ImVec2(imageMin.x + 8.0f, imageMin.y + 8.0f), IM_COL32(255, 255, 255, 220), "CPU");
				drawList->AddText(ImVec2(dividerX + 8.0f, imageMin.y + 8.0f), IM_COL32(255, 255, 255, 220), "GPU");

				// Dragging near the divider moves it instead of picking
				bool nearDivider = hovered && std::abs(ImGui::GetMousePos().x - dividerX) < 6.0f;
				if (nearDivider || m_DraggingDivider)
					ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
				if (nearDivider && clicked) {
					m_DraggingDivider = true;
					clicked = false;
				}
				if (m_DraggingDivider) {
					if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
						settings.CompareSplit = glm::clamp((ImGui::GetMousePos().x - imageMin.x) / imageSize.x, 0.0f, 1.0f);
					else
						m_DraggingDivider = false;
				}
			}

			// Left click selects, Ctrl+left click focuses the camera on the point under the cursor.
			// (Right mouse is camera look, so ignore clicks while it's held.)
			if (clicked) {
				ImVec2 mouse = ImGui::GetMousePos();
				// The image is drawn flipped (row 0 = bottom), so NDC y points up
				glm::vec2 ndc = { (mouse.x - imageMin.x) / imageSize.x * 2.0f - 1.0f, 1.0f - (mouse.y - imageMin.y) / imageSize.y * 2.0f };

				float distance = 0.0f;
				ObjectRef hit = m_Renderer.Pick(m_Scene, m_Camera, ndc, &distance);
				if (ImGui::GetIO().KeyCtrl) {
					if (hit.IsValid()) {
						// Focus distance is measured along the view direction, not along the ray
						float depth = distance * glm::dot(m_Camera.GetRayDirection(ndc), m_Camera.GetDirection());
						m_Camera.SetFocusDistance(std::max(depth, 0.01f));
						m_Renderer.ResetFrameIndex();
					}
				}
				else {
					m_Selection = hit;
				}
			}

			DrawSelectionOutline(imageMin, imageSize);
		}

		ImGui::End();
		ImGui::PopStyleVar();
	}

	// Outlines the selected object's projected bounding box over the viewport image
	void DrawSelectionOutline(const ImVec2& imageMin, const ImVec2& imageSize) {
		glm::vec3 min, max;
		if (!m_Scene.GetBounds(m_Selection, min, max))
			return;

		glm::mat4 viewProjection = m_Camera.GetProjection() * m_Camera.GetView();
		glm::vec2 screenMin(FLT_MAX), screenMax(-FLT_MAX);
		for (int corner = 0; corner < 8; corner++) {
			glm::vec3 point = { corner & 1 ? max.x : min.x, corner & 2 ? max.y : min.y, corner & 4 ? max.z : min.z };
			glm::vec4 clip = viewProjection * glm::vec4(point, 1.0f);
			if (clip.w <= 0.0001f)
				return; // part of the box is behind the camera: its projection isn't meaningful
			glm::vec2 ndc = glm::vec2(clip) / clip.w;
			glm::vec2 screen = { imageMin.x + (ndc.x * 0.5f + 0.5f) * imageSize.x, imageMin.y + (0.5f - ndc.y * 0.5f) * imageSize.y };
			screenMin = glm::min(screenMin, screen);
			screenMax = glm::max(screenMax, screen);
		}

		ImDrawList* drawList = ImGui::GetWindowDrawList();
		drawList->PushClipRect(imageMin, ImVec2(imageMin.x + imageSize.x, imageMin.y + imageSize.y), true);
		drawList->AddRect(ImVec2(screenMin.x, screenMin.y), ImVec2(screenMax.x, screenMax.y), IM_COL32(255, 200, 0, 220), 0.0f, 0, 1.5f);
		drawList->PopClipRect();
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

	ObjectRef m_Selection;
	bool m_DraggingDivider = false; // CompareMode::Split
	bool m_SceneChanged = false; // set by any edit during the current frame

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
