#include "Mesh.h"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cfloat>

void Mesh::ComputeBounds() {
	if (Vertices.empty()) {
		BoundsMin = BoundsMax = glm::vec3(0.0f);
		return;
	}
	BoundsMin = glm::vec3(FLT_MAX);
	BoundsMax = glm::vec3(-FLT_MAX);
	for (const Vertex& vertex : Vertices) {
		BoundsMin = glm::min(BoundsMin, vertex.Position);
		BoundsMax = glm::max(BoundsMax, vertex.Position);
	}
}

namespace BuiltinMeshes {

	// Adds a unit square facing `normal`, centered at `center`; u x v must equal normal
	static void AddFace(Mesh& mesh, const glm::vec3& center, const glm::vec3& normal, const glm::vec3& u, const glm::vec3& v) {
		uint32_t base = (uint32_t)mesh.Vertices.size();
		mesh.Vertices.push_back({ center - 0.5f * u - 0.5f * v, normal });
		mesh.Vertices.push_back({ center + 0.5f * u - 0.5f * v, normal });
		mesh.Vertices.push_back({ center + 0.5f * u + 0.5f * v, normal });
		mesh.Vertices.push_back({ center - 0.5f * u + 0.5f * v, normal });
		for (uint32_t index : { 0u, 1u, 2u, 0u, 2u, 3u })
			mesh.Indices.push_back(base + index);
	}

	static void CreateBox(Mesh& mesh) {
		const glm::vec3 x(1, 0, 0), y(0, 1, 0), z(0, 0, 1);
		AddFace(mesh, 0.5f * x, x, y, z);
		AddFace(mesh, -0.5f * x, -x, z, y);
		AddFace(mesh, 0.5f * y, y, z, x);
		AddFace(mesh, -0.5f * y, -y, x, z);
		AddFace(mesh, 0.5f * z, z, x, y);
		AddFace(mesh, -0.5f * z, -z, y, x);
	}

	static void CreateQuad(Mesh& mesh) {
		AddFace(mesh, glm::vec3(0.0f), glm::vec3(0, 1, 0), glm::vec3(0, 0, 1), glm::vec3(1, 0, 0));
	}

	static void CreateSphere(Mesh& mesh, uint32_t segments = 48, uint32_t rings = 24) {
		for (uint32_t ring = 0; ring <= rings; ring++) {
			float theta = glm::pi<float>() * (float)ring / (float)rings;
			for (uint32_t segment = 0; segment <= segments; segment++) {
				float phi = 2.0f * glm::pi<float>() * (float)segment / (float)segments;
				glm::vec3 normal(glm::sin(theta) * glm::cos(phi), glm::cos(theta), glm::sin(theta) * glm::sin(phi));
				mesh.Vertices.push_back({ 0.5f * normal, normal });
			}
		}
		for (uint32_t ring = 0; ring < rings; ring++) {
			for (uint32_t segment = 0; segment < segments; segment++) {
				uint32_t a = ring * (segments + 1) + segment;
				uint32_t b = a + segments + 1;
				// Skip the triangles that collapse to a point at the poles
				if (ring > 0)
					mesh.Indices.insert(mesh.Indices.end(), { a, a + 1, b });
				if (ring < rings - 1)
					mesh.Indices.insert(mesh.Indices.end(), { a + 1, b + 1, b });
			}
		}
	}

	bool IsBuiltin(const std::string& source) {
		return source.rfind("builtin:", 0) == 0;
	}

	bool Create(const std::string& source, Mesh& mesh) {
		mesh = Mesh();
		mesh.Source = source;
		if (source == Box) {
			mesh.Name = "Box";
			CreateBox(mesh);
		}
		else if (source == Quad) {
			mesh.Name = "Quad";
			CreateQuad(mesh);
		}
		else if (source == Sphere) {
			mesh.Name = "Sphere Mesh";
			CreateSphere(mesh);
		}
		else {
			return false;
		}
		mesh.ComputeBounds();
		return true;
	}

}
