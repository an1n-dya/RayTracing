#include "Renderer.h"

#include "BSDF.h"
#include "EnvironmentSampling.h"
#include "ImageExport.h"
#include "Intersection.h"
#include "LightSampling.h"
#include "ToneMapping.h"

#include "Walnut/Random.h"

#include <execution>
#include <filesystem>

namespace Utils {
	static uint32_t ConvertToRGBA(const glm::vec4& color) {
		// Round to nearest, like the GPU's UNORM image stores
		uint8_t r = static_cast<uint8_t>(color.r * 255.0f + 0.5f);
		uint8_t g = static_cast<uint8_t>(color.g * 255.0f + 0.5f);
		uint8_t b = static_cast<uint8_t>(color.b * 255.0f + 0.5f);
		uint8_t a = static_cast<uint8_t>(color.a * 255.0f + 0.5f);

		return (a << 24) | (b << 16) | (g << 8) | r;
	}

	static uint32_t PCG_Hash(uint32_t input) {
		uint32_t state = input * 747796405u + 2891336453u;
		uint32_t word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
		return (word >> 22u) ^ word;
	}

	// Uniform in [0, 1): the top 24 bits fit a float's mantissa exactly, so the result can never round up to 1
	static float RandomFloat(uint32_t& seed) {
		seed = PCG_Hash(seed);
		return (float)(seed >> 8) * (1.0f / 16777216.0f);
	}

#define MT 1
	// Runs fn(x, y) for every pixel, in parallel unless MT is 0
	template<typename Fn>
	static void ForEachPixel(const std::vector<uint32_t>& xs, const std::vector<uint32_t>& ys, Fn&& fn) {
#if MT
		std::for_each(std::execution::par, ys.begin(), ys.end(), [&](uint32_t y) {
			std::for_each(std::execution::par, xs.begin(), xs.end(), [&](uint32_t x) { fn(x, y); });
		});
#else
		for (uint32_t y : ys)
			for (uint32_t x : xs)
				fn(x, y);
#endif
	}
}

void Renderer::OnResize(uint32_t width, uint32_t height) {
	m_GpuPathTracer.OnResize(width, height);

	if (m_FinalImage) {
		if (m_FinalImage->GetWidth() == width && m_FinalImage->GetHeight() == height)
			return;
		m_FinalImage->Resize(width, height);
	}
	else {
		m_FinalImage = std::make_shared<Walnut::Image>(width, height, Walnut::ImageFormat::RGBA);
	}

	// The (re)allocated accumulation buffers hold garbage until frame 1 clears them
	m_FrameIndex = 1;

	delete[] m_ImageData;
	m_ImageData = new uint32_t[width * height];

	delete[] m_AccumulationData;
	m_AccumulationData = new glm::vec4[width * height];
		
	m_ImageHorizontalIter.resize(width);
	m_ImageVerticalIter.resize(height);
	for (uint32_t i = 0; i < width; i++)
		m_ImageHorizontalIter[i] = i;
	for (uint32_t i = 0; i < height; i++)
		m_ImageVerticalIter[i] = i;
}

bool Renderer::IsConverged() const {
	return m_Settings.Accumulate && m_Settings.MaxSamples > 0 && m_FrameIndex > 1
		&& m_AccumulatedSamples >= (uint32_t)m_Settings.MaxSamples;
}

bool Renderer::Render(const Scene& scene, const Camera& camera) {
	if (!m_FinalImage || !m_ImageData)
		return false;

	PrepareScene(scene);
	m_ActiveCamera = &camera;

	if (m_FrameIndex == 1)
		m_AccumulatedSamples = 0;

	FrameParams frame;
	frame.ResetAccumulation = m_FrameIndex == 1;
	frame.FirstSampleIndex = m_AccumulatedSamples;
	frame.SampleCount = (uint32_t)std::max(m_Settings.SamplesPerFrame, 1);
	if (m_Settings.Accumulate && m_Settings.MaxSamples > 0) // don't overshoot the limit
		frame.SampleCount = std::min(frame.SampleCount, (uint32_t)m_Settings.MaxSamples - std::min(m_AccumulatedSamples, (uint32_t)m_Settings.MaxSamples));
	// Once converged SampleCount is 0: only re-resolve the accumulated image (so exposure/tone mapping stay live)

	// Compare modes run both paths on exactly the same samples
	bool comparing = m_Settings.Compare != CompareMode::Off;
	if (m_Settings.UseGPU || comparing)
		m_GpuPathTracer.Render(scene, m_Prepared, camera, m_Settings, frame);
	if (!m_Settings.UseGPU || comparing)
		RenderCPU(frame);
	if (m_Settings.Compare == CompareMode::Difference)
		UpdateDifferenceImage();

	if (frame.SampleCount == 0)
		return false;

	m_AccumulatedSamples += frame.SampleCount;

	if (m_Settings.Accumulate)
		m_FrameIndex++;
	else
		m_FrameIndex = 1;

	return true;
}

glm::vec4 Renderer::PerPixel(uint32_t x, uint32_t y, uint32_t sampleIndex) {
	// Decorrelate pixels and samples: every (pixel, sample) pair gets its own random stream
	uint32_t seed = Utils::PCG_Hash((x + y * m_FinalImage->GetWidth()) ^ Utils::PCG_Hash(sampleIndex));

	glm::vec2 jitter(0.5f);
	if (m_Settings.AntiAliasing)
		jitter = { RandomFloat(seed), RandomFloat(seed) };

	glm::vec2 ndc = (glm::vec2((float)x, (float)y) + jitter) / glm::vec2((float)m_FinalImage->GetWidth(), (float)m_FinalImage->GetHeight());
	ndc = ndc * 2.0f - 1.0f; // -1 -> 1

	glm::vec2 lensSample(0.0f);
	if (m_ActiveCamera->GetAperture() > 0.0f)
		lensSample = { RandomFloat(seed), RandomFloat(seed) };

	Ray ray = m_ActiveCamera->GenerateRay(ndc, lensSample);

	glm::vec3 light(0.0f);
	glm::vec3 contribution(1.0f);

	bool lightSampling = m_Settings.LightSampling && m_Prepared.HasLights();
	float previousPdf = 0.0f;   // pdf of the BSDF sample that led to the current hit
	bool previousDelta = true;  // ...or it came from a delta lobe / the camera, which light sampling can't produce

	for (int i = 0; i < m_Settings.MaxBounces; i++) {
		Renderer::HitPayload payload = TraceRay(ray);
		if (payload.HitDistance < 0.0f) {
			// An environment map is also a light that light sampling aims at: share it via MIS
			float weight = 1.0f;
			if (lightSampling && !previousDelta && m_Prepared.EnvironmentProbability > 0.0f) {
				float lightPdf = m_Prepared.EnvironmentProbability * EnvironmentSampling::Pdf(*m_Prepared.Environment, ray.Direction, m_Prepared.EnvironmentRotation);
				weight = LightSampling::PowerHeuristic(previousPdf, lightPdf);
			}
			light += m_ActiveScene->Sky.GetRadiance(ray.Direction) * contribution * weight;
			break;
		}

		const Material& material = m_ActiveScene->Materials[payload.MaterialIndex];

		// Only the front face of a surface emits (the side its normal points to)
		glm::vec3 wo = -ray.Direction;
		bool frontFace = glm::dot(payload.GeometricNormal, wo) >= 0.0f;

		// Emission is weighted by the throughput accumulated *before* this surface scatters the path. If light
		// sampling at the previous bounce could also have found this light, the two strategies share it via MIS.
		glm::vec3 emission = material.GetEmission();
		if (frontFace && (emission.r > 0.0f || emission.g > 0.0f || emission.b > 0.0f)) {
			float weight = 1.0f;
			if (lightSampling && !previousDelta) {
				float lightPdf = LightPdf(payload, ray);
				if (lightPdf > 0.0f)
					weight = LightSampling::PowerHeuristic(previousPdf, lightPdf);
			}
			light += emission * contribution * weight;
		}

		// Shade with the normals facing the incoming ray (a ray can hit a surface from behind, e.g. from inside glass)
		glm::vec3 geometricNormal = frontFace ? payload.GeometricNormal : -payload.GeometricNormal;
		glm::vec3 normal = frontFace ? payload.WorldNormal : -payload.WorldNormal;
		// Interpolated normals can face away from the viewer near silhouettes: use the true one there
		if (glm::dot(normal, wo) <= 0.0f)
			normal = geometricNormal;

		// Not at the last bounce: the BSDF-sampled ray it competes with (via MIS) would never be traced.
		// Pure glass has nothing light sampling could contribute to (its lobe is delta-like).
		if (lightSampling && i + 1 < m_Settings.MaxBounces && BSDF::GlassWeight(material) < 1.0f)
			light += contribution * SampleDirectLight(payload.WorldPosition + geometricNormal * 0.0001f, normal, geometricNormal, wo, material, seed);

		// Draw the random numbers one at a time so the order matches the GPU path
		glm::vec3 u;
		u.x = RandomFloat(seed);
		u.y = RandomFloat(seed);
		u.z = RandomFloat(seed);

		BSDF::Sample bsdfSample;
		if (!BSDF::SampleDirection(material, normal, wo, frontFace, u, bsdfSample))
			break;
		// A reflection that dips below the actual surface (possible with interpolated normals) would leak light
		bool leavesAbove = glm::dot(bsdfSample.Direction, geometricNormal) > 0.0f;
		if (!bsdfSample.Delta && !leavesAbove)
			break;
		contribution *= bsdfSample.Weight;

		previousPdf = bsdfSample.Pdf;
		previousDelta = bsdfSample.Delta;

		// Nudge the origin off the surface, to whichever side the new ray leaves on (transmission goes through)
		ray.Origin = payload.WorldPosition + geometricNormal * (leavesAbove ? 0.0001f : -0.0001f);
		ray.Direction = bsdfSample.Direction;

		// Russian roulette: randomly end paths that can't carry much more light, boosting the
		// survivors by 1/p so the estimate stays unbiased.
		if (m_Settings.RussianRoulette && i >= m_Settings.RussianRouletteStartBounce) {
			float p = glm::clamp(glm::max(contribution.r, glm::max(contribution.g, contribution.b)), 0.05f, 1.0f);
			if (RandomFloat(seed) > p)
				break;
			contribution /= p;
		}
	}

	return glm::vec4(light, 1.0f);
}

float Renderer::RandomFloat(uint32_t& seed) const {
	if (m_Settings.SlowRandom)
		return Walnut::Random::Float();
	return Utils::RandomFloat(seed);
}

void Renderer::PrepareScene(const Scene& scene) {
	m_ActiveScene = &scene;
	m_Prepared.Prepare(scene);
}

glm::vec3 Renderer::SampleDirectLight(const glm::vec3& origin, const glm::vec3& normal, const glm::vec3& geometricNormal,
	const glm::vec3& wo, const Material& material, uint32_t& seed)
{
	// Always draw all three numbers so the random sequence doesn't depend on what happens below
	float uLight = RandomFloat(seed);
	float u1 = RandomFloat(seed);
	float u2 = RandomFloat(seed);

	float environmentProbability = m_Prepared.EnvironmentProbability;
	if (uLight < environmentProbability) {
		glm::vec3 direction;
		float pdf;
		if (!EnvironmentSampling::Sample(*m_Prepared.Environment, m_Prepared.EnvironmentRotation, u1, u2, direction, pdf))
			return glm::vec3(0.0f);
		if (glm::dot(direction, geometricNormal) <= 0.0f)
			return glm::vec3(0.0f);

		glm::vec3 f = BSDF::Evaluate(material, normal, wo, direction);
		if (f.r <= 0.0f && f.g <= 0.0f && f.b <= 0.0f)
			return glm::vec3(0.0f);

		// Visible if the shadow ray escapes the scene
		Ray shadowRay;
		shadowRay.Origin = origin;
		shadowRay.Direction = direction;
		if (TraceRay(shadowRay).HitDistance >= 0.0f)
			return glm::vec3(0.0f);

		float lightPdf = environmentProbability * pdf;
		float weight = LightSampling::PowerHeuristic(lightPdf, BSDF::Pdf(material, normal, wo, direction));
		glm::vec3 radiance = EnvironmentSampling::Lookup(*m_Prepared.Environment, direction, m_Prepared.EnvironmentRotation) * m_Prepared.EnvironmentIntensity;
		return f * radiance * (weight / lightPdf);
	}

	if (m_Prepared.Lights.empty())
		return glm::vec3(0.0f);

	// Reuse uLight (rescaled to [0,1)) to pick among the scene's lights
	const PreparedScene::Light& light = m_Prepared.Lights[m_Prepared.SelectLight((uLight - environmentProbability) / (1.0f - environmentProbability))];

	glm::vec3 direction;
	float pdf;
	if (light.Type == PreparedScene::LightType::Sphere) {
		if (!LightSampling::SampleSphere(origin, light.Position0, light.Radius, u1, u2, direction, pdf))
			return glm::vec3(0.0f);
	}
	else {
		float distance;
		if (!LightSampling::SampleTriangle(origin, light.Position0, light.Position1, light.Position2, light.Normal, light.Area, u1, u2, direction, distance, pdf))
			return glm::vec3(0.0f);
	}

	if (glm::dot(direction, geometricNormal) <= 0.0f)
		return glm::vec3(0.0f); // the light is behind the surface

	glm::vec3 f = BSDF::Evaluate(material, normal, wo, direction);
	if (f.r <= 0.0f && f.g <= 0.0f && f.b <= 0.0f)
		return glm::vec3(0.0f);

	// Shadow ray: the light is visible if it's the first thing the ray hits
	Ray shadowRay;
	shadowRay.Origin = origin;
	shadowRay.Direction = direction;
	HitPayload hit = TraceRay(shadowRay);
	bool visible = light.Type == PreparedScene::LightType::Sphere
		? hit.Object == ObjectRef{ ObjectType::Sphere, (int)light.ObjectIndex }
		: hit.Object == ObjectRef{ ObjectType::MeshInstance, (int)light.ObjectIndex } && hit.Triangle == light.Triangle;
	if (!visible)
		return glm::vec3(0.0f);

	float lightPdf = (1.0f - environmentProbability) * light.Probability * pdf;
	float weight = LightSampling::PowerHeuristic(lightPdf, BSDF::Pdf(material, normal, wo, direction));
	return f * light.Emission * (weight / lightPdf);
}

float Renderer::LightPdf(const HitPayload& hit, const Ray& ray) const {
	if (hit.Object.Type == ObjectType::Sphere) {
		int lightIndex = m_Prepared.SphereLights[hit.Object.Index];
		if (lightIndex < 0)
			return 0.0f;
		const PreparedScene::Light& light = m_Prepared.Lights[lightIndex];
		return (1.0f - m_Prepared.EnvironmentProbability) * light.Probability * LightSampling::SpherePdf(ray.Origin, light.Position0, light.Radius);
	}
	if (hit.Object.Type == ObjectType::MeshInstance) {
		int lightOffset = m_Prepared.InstanceLightOffsets[hit.Object.Index];
		if (lightOffset < 0)
			return 0.0f;
		const PreparedScene::Light& light = m_Prepared.Lights[lightOffset + hit.Triangle];
		float cosLight = glm::abs(glm::dot(hit.GeometricNormal, ray.Direction));
		return (1.0f - m_Prepared.EnvironmentProbability) * light.Probability * LightSampling::TrianglePdf(hit.HitDistance, cosLight, light.Area);
	}
	return 0.0f; // planes aren't in the light list
}

Renderer::HitPayload Renderer::TraceRay(const Ray& ray) {
	const Scene& scene = *m_ActiveScene;

	float hitDistance = std::numeric_limits<float>::max();
	ObjectRef hitObject;
	uint32_t hitTriangle = 0;
	glm::vec2 hitBarycentrics(0.0f);

	for (size_t i = 0; i < scene.Spheres.size(); i++) {
		float t = Intersect::Sphere(ray.Origin, ray.Direction, scene.Spheres[i].Position, scene.Spheres[i].Radius);
		if (t > 0.0f && t < hitDistance) {
			hitDistance = t;
			hitObject = { ObjectType::Sphere, (int)i };
		}
	}

	for (size_t i = 0; i < scene.Planes.size(); i++) {
		float t = Intersect::Plane(ray.Origin, ray.Direction, scene.Planes[i].Point, scene.Planes[i].Normal);
		if (t > 0.0f && t < hitDistance) {
			hitDistance = t;
			hitObject = { ObjectType::Plane, (int)i };
		}
	}

	for (size_t i = 0; i < scene.MeshInstances.size(); i++) {
		const Mesh& mesh = scene.Meshes[scene.MeshInstances[i].MeshIndex];
		const PreparedScene::Instance& instance = m_Prepared.Instances[i];

		const std::vector<BVHNode>& nodes = mesh.BVHNodes;
		if (nodes.empty())
			continue;

		// Intersect in object space. The direction isn't renormalized, so distances stay comparable with world space.
		glm::vec3 origin = glm::vec3(instance.WorldToObject * glm::vec4(ray.Origin, 1.0f));
		glm::vec3 direction = glm::mat3(instance.WorldToObject) * ray.Direction;
		glm::vec3 inverseDirection = 1.0f / direction;

		// Distance to a node's box, or -1 if the ray misses it (or only hits it beyond the closest hit so far)
		const glm::vec3 padding(1e-4f); // flat meshes (quads) have zero-thickness bounds
		auto nodeDistance = [&](uint32_t nodeIndex) {
			return Intersect::AABB(origin, inverseDirection, nodes[nodeIndex].BoundsMin - padding, nodes[nodeIndex].BoundsMax + padding, hitDistance);
		};

		if (nodeDistance(0) < 0.0f)
			continue;

		// Stack-based BVH traversal, nearer child first
		uint32_t stack[64];
		uint32_t stackSize = 0;
		uint32_t nodeIndex = 0;
		while (true) {
			const BVHNode& node = nodes[nodeIndex];
			if (node.IsLeaf()) {
				for (uint32_t triangle = node.LeftOrFirst; triangle < node.LeftOrFirst + node.TriangleCount; triangle++) {
					const glm::vec3& v0 = mesh.Vertices[mesh.Indices[triangle * 3 + 0]].Position;
					const glm::vec3& v1 = mesh.Vertices[mesh.Indices[triangle * 3 + 1]].Position;
					const glm::vec3& v2 = mesh.Vertices[mesh.Indices[triangle * 3 + 2]].Position;
					glm::vec2 barycentrics;
					float t = Intersect::Triangle(origin, direction, v0, v1, v2, barycentrics);
					if (t > 0.0f && t < hitDistance) {
						hitDistance = t;
						hitObject = { ObjectType::MeshInstance, (int)i };
						hitTriangle = triangle;
						hitBarycentrics = barycentrics;
					}
				}
				if (stackSize == 0)
					break;
				nodeIndex = stack[--stackSize];
				continue;
			}

			uint32_t nearChild = node.LeftOrFirst, farChild = node.LeftOrFirst + 1;
			float nearDistance = nodeDistance(nearChild), farDistance = nodeDistance(farChild);
			if (nearDistance < 0.0f || (farDistance >= 0.0f && farDistance < nearDistance)) {
				std::swap(nearChild, farChild);
				std::swap(nearDistance, farDistance);
			}

			if (nearDistance < 0.0f) {
				if (stackSize == 0)
					break;
				nodeIndex = stack[--stackSize];
				continue;
			}
			nodeIndex = nearChild;
			if (farDistance >= 0.0f)
				stack[stackSize++] = farChild;
		}
	}

	if (!hitObject.IsValid())
		return Miss(ray);

	return ClosestHit(ray, hitDistance, hitObject, hitTriangle, hitBarycentrics);
}

Renderer::HitPayload Renderer::ClosestHit(const Ray& ray, float hitDistance, const ObjectRef& object, uint32_t triangle, const glm::vec2& barycentrics) {
	const Scene& scene = *m_ActiveScene;

	Renderer::HitPayload payload;
	payload.HitDistance = hitDistance;
	payload.Object = object;
	payload.Triangle = triangle;
	payload.MaterialIndex = scene.GetMaterialIndex(object);
	payload.WorldPosition = ray.Origin + ray.Direction * hitDistance;

	switch (object.Type) {
	case ObjectType::Sphere:
		payload.WorldNormal = glm::normalize(payload.WorldPosition - scene.Spheres[object.Index].Position);
		payload.GeometricNormal = payload.WorldNormal;
		break;
	case ObjectType::Plane:
		payload.WorldNormal = scene.Planes[object.Index].Normal;
		payload.GeometricNormal = payload.WorldNormal;
		break;
	case ObjectType::MeshInstance: {
		const Mesh& mesh = scene.Meshes[scene.MeshInstances[object.Index].MeshIndex];
		const PreparedScene::Instance& instance = m_Prepared.Instances[object.Index];
		const Vertex& a = mesh.Vertices[mesh.Indices[triangle * 3 + 0]];
		const Vertex& b = mesh.Vertices[mesh.Indices[triangle * 3 + 1]];
		const Vertex& c = mesh.Vertices[mesh.Indices[triangle * 3 + 2]];

		glm::vec3 shading = (1.0f - barycentrics.x - barycentrics.y) * a.Normal + barycentrics.x * b.Normal + barycentrics.y * c.Normal;
		payload.WorldNormal = glm::normalize(instance.NormalMatrix * shading);
		payload.GeometricNormal = glm::normalize(instance.NormalMatrix * glm::cross(b.Position - a.Position, c.Position - a.Position));
		// Trust the authored normals about which side is the outside (winding in files isn't always consistent)
		if (glm::dot(payload.GeometricNormal, payload.WorldNormal) < 0.0f)
			payload.GeometricNormal = -payload.GeometricNormal;
		break;
	}
	default:
		break;
	}

	return payload;
}

Renderer::HitPayload Renderer::Miss(const Ray& ray) {
	Renderer::HitPayload payload;
	payload.HitDistance = -0.1f;
	return payload;
}

void Renderer::RenderCPU(const FrameParams& frame) {
	uint32_t width = m_FinalImage->GetWidth();
	if (frame.ResetAccumulation)
		memset(m_AccumulationData, 0, width * m_FinalImage->GetHeight() * sizeof(glm::vec4));

	Utils::ForEachPixel(m_ImageHorizontalIter, m_ImageVerticalIter, [this, &frame, width](uint32_t x, uint32_t y) {
		uint32_t index = x + y * width;
		for (uint32_t s = 0; s < frame.SampleCount; s++)
			m_AccumulationData[index] += PerPixel(x, y, frame.FirstSampleIndex + s);
		m_ImageData[index] = Utils::ConvertToRGBA(ToneMapping::Resolve(m_AccumulationData[index], m_Settings));
	});

	m_FinalImage->SetData(m_ImageData);
}

void Renderer::UpdateDifferenceImage() {
	uint32_t width = m_FinalImage->GetWidth();
	uint32_t height = m_FinalImage->GetHeight();
	if (m_GpuPathTracer.GetWidth() != width || m_GpuPathTracer.GetHeight() != height)
		return;

	if (!m_DifferenceImage)
		m_DifferenceImage = std::make_shared<Walnut::Image>(width, height, Walnut::ImageFormat::RGBA);
	else if (m_DifferenceImage->GetWidth() != width || m_DifferenceImage->GetHeight() != height)
		m_DifferenceImage->Resize(width, height);

	m_GpuPathTracer.ReadDisplayImage(m_GPUPixels);
	m_DifferenceData.resize((size_t)width * height);

	float scale = m_Settings.DifferenceScale;
	Utils::ForEachPixel(m_ImageHorizontalIter, m_ImageVerticalIter, [&](uint32_t x, uint32_t y) {
		uint32_t index = x + y * width;
		uint32_t cpu = m_ImageData[index], gpu = m_GPUPixels[index];
		uint32_t result = 0xff000000;
		for (int shift = 0; shift < 24; shift += 8) {
			int difference = std::abs((int)((cpu >> shift) & 0xff) - (int)((gpu >> shift) & 0xff));
			result |= (uint32_t)std::min((int)(difference * scale + 0.5f), 255) << shift;
		}
		m_DifferenceData[index] = result;
	});

	// Mean error in a serial pass (cheap next to tracing, and avoids sharing a sum across threads)
	uint64_t totalError = 0;
	for (size_t i = 0; i < m_DifferenceData.size(); i++) {
		uint32_t cpu = m_ImageData[i], gpu = m_GPUPixels[i];
		for (int shift = 0; shift < 24; shift += 8)
			totalError += (uint64_t)std::abs((int)((cpu >> shift) & 0xff) - (int)((gpu >> shift) & 0xff));
	}
	m_CompareMeanError = (float)((double)totalError / (3.0 * (double)m_DifferenceData.size()));

	m_DifferenceImage->SetData(m_DifferenceData.data());
}

ObjectRef Renderer::Pick(const Scene& scene, const Camera& camera, const glm::vec2& ndc, float* outDistance) {
	PrepareScene(scene);

	Ray ray;
	ray.Origin = camera.GetPosition();
	ray.Direction = camera.GetRayDirection(ndc);

	HitPayload payload = TraceRay(ray);
	if (payload.HitDistance < 0.0f)
		return {};

	if (outDistance)
		*outDistance = payload.HitDistance;
	return payload.Object;
}

bool Renderer::SaveImage(const std::string& path) {
	uint32_t width = GetFinalImageWidth();
	uint32_t height = GetFinalImageHeight();
	if (width == 0 || height == 0)
		return false;
	size_t pixelCount = (size_t)width * height;

	std::string extension = std::filesystem::path(path).extension().string();
	for (char& c : extension)
		c = (char)tolower(c);

	if (extension == ".hdr") {
		std::vector<glm::vec4> accumulation;
		if (m_Settings.UseGPU)
			m_GpuPathTracer.ReadAccumulationImage(accumulation);
		else
			accumulation.assign(m_AccumulationData, m_AccumulationData + pixelCount);

		std::vector<float> rgb(pixelCount * 3);
		for (size_t i = 0; i < pixelCount; i++) {
			glm::vec3 average = accumulation[i].a > 0.0f ? glm::vec3(accumulation[i]) / accumulation[i].a : glm::vec3(0.0f);
			rgb[i * 3 + 0] = average.r;
			rgb[i * 3 + 1] = average.g;
			rgb[i * 3 + 2] = average.b;
		}
		return ImageExport::SaveHDR(path, width, height, rgb.data());
	}

	std::vector<uint32_t> pixels;
	if (m_Settings.UseGPU)
		m_GpuPathTracer.ReadDisplayImage(pixels);
	else
		pixels.assign(m_ImageData, m_ImageData + pixelCount);
	return ImageExport::SavePNG(path, width, height, pixels.data());
}

VkDescriptorSet Renderer::GetFinalImageDescriptorSet() const {
	if (m_Settings.Compare == CompareMode::Difference && m_DifferenceImage)
		return m_DifferenceImage->GetDescriptorSet();
	if (m_Settings.UseGPU)
		return m_GpuPathTracer.GetDescriptorSet();
	return m_FinalImage ? m_FinalImage->GetDescriptorSet() : nullptr;
}

uint32_t Renderer::GetFinalImageWidth() const {
	if (m_Settings.UseGPU)
		return m_GpuPathTracer.GetWidth();
	return m_FinalImage ? m_FinalImage->GetWidth() : 0;
}

uint32_t Renderer::GetFinalImageHeight() const {
	if (m_Settings.UseGPU)
		return m_GpuPathTracer.GetHeight();
	return m_FinalImage ? m_FinalImage->GetHeight() : 0;
}
