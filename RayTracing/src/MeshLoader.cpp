#include "MeshLoader.h"

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>

namespace MeshLoader {

	namespace {

		// Parses one "v", "v/vt", "v//vn" or "v/vt/vn" face corner into 0-based position / normal indices
		// (normal = -1 when absent). OBJ indices are 1-based; negative ones count back from the end.
		bool ParseCorner(const char*& cursor, int positionCount, int normalCount, int& position, int& normal) {
			char* end = nullptr;
			long value = strtol(cursor, &end, 10);
			if (end == cursor || value == 0)
				return false;
			position = value > 0 ? (int)value - 1 : positionCount + (int)value;
			cursor = end;
			normal = -1;

			if (*cursor == '/') {
				cursor++;
				if (*cursor != '/') { // skip the texture coordinate index
					strtol(cursor, &end, 10);
					cursor = end;
				}
				if (*cursor == '/') {
					cursor++;
					value = strtol(cursor, &end, 10);
					if (end != cursor && value != 0) {
						normal = value > 0 ? (int)value - 1 : normalCount + (int)value;
						cursor = end;
					}
				}
			}
			return position >= 0 && position < positionCount && normal < normalCount;
		}

		bool LoadOBJ(const std::string& path, Mesh& mesh, std::string* error) {
			std::ifstream file(path);
			if (!file) {
				if (error)
					*error = "can't open " + path;
				return false;
			}

			std::vector<glm::vec3> positions, normals;
			// Vertices are unique (position, normal) pairs; corners without a normal get smooth normals below
			std::unordered_map<uint64_t, uint32_t> vertexLookup;
			bool needsGeneratedNormals = false;

			std::string line;
			int lineNumber = 0;
			while (std::getline(file, line)) {
				lineNumber++;
				const char* cursor = line.c_str();
				while (*cursor == ' ' || *cursor == '\t')
					cursor++;

				if (cursor[0] == 'v' && (cursor[1] == ' ' || cursor[1] == '\t')) {
					glm::vec3 p;
					char* end;
					p.x = strtof(cursor + 2, &end);
					p.y = strtof(end, &end);
					p.z = strtof(end, &end);
					positions.push_back(p);
				}
				else if (cursor[0] == 'v' && cursor[1] == 'n' && (cursor[2] == ' ' || cursor[2] == '\t')) {
					glm::vec3 n;
					char* end;
					n.x = strtof(cursor + 3, &end);
					n.y = strtof(end, &end);
					n.z = strtof(end, &end);
					normals.push_back(glm::dot(n, n) > 0.0f ? glm::normalize(n) : glm::vec3(0, 1, 0));
				}
				else if (cursor[0] == 'f' && (cursor[1] == ' ' || cursor[1] == '\t')) {
					cursor += 2;
					uint32_t corners[64];
					int cornerCount = 0;
					while (true) {
						while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r')
							cursor++;
						if (*cursor == '\0')
							break;
						int position, normal;
						if (!ParseCorner(cursor, (int)positions.size(), (int)normals.size(), position, normal)) {
							if (error)
								*error = path + ":" + std::to_string(lineNumber) + ": invalid face";
							return false;
						}
						needsGeneratedNormals |= normal < 0;

						uint64_t key = ((uint64_t)(uint32_t)position << 32) | (uint32_t)(normal + 1);
						auto [it, inserted] = vertexLookup.try_emplace(key, (uint32_t)mesh.Vertices.size());
						if (inserted)
							mesh.Vertices.push_back({ positions[position], normal >= 0 ? normals[normal] : glm::vec3(0.0f) });
						if (cornerCount < 64)
							corners[cornerCount++] = it->second;
					}
					// Triangulate the polygon as a fan
					for (int i = 1; i + 1 < cornerCount; i++)
						mesh.Indices.insert(mesh.Indices.end(), { corners[0], corners[i], corners[i + 1] });
				}
				// Everything else (vt, g, o, s, usemtl, mtllib, comments, ...) is ignored
			}

			if (mesh.Indices.empty()) {
				if (error)
					*error = path + ": no faces found";
				return false;
			}

			if (needsGeneratedNormals) {
				// Area-weighted smooth normals, accumulated per position so they're shared across faces
				std::vector<glm::vec3> positionNormals(positions.size(), glm::vec3(0.0f));
				std::vector<int> vertexPosition(mesh.Vertices.size());
				for (const auto& [key, vertex] : vertexLookup)
					vertexPosition[vertex] = (int)(key >> 32);

				for (size_t i = 0; i + 2 < mesh.Indices.size(); i += 3) {
					const glm::vec3& a = mesh.Vertices[mesh.Indices[i]].Position;
					const glm::vec3& b = mesh.Vertices[mesh.Indices[i + 1]].Position;
					const glm::vec3& c = mesh.Vertices[mesh.Indices[i + 2]].Position;
					glm::vec3 faceNormal = glm::cross(b - a, c - a); // length = 2 * area
					for (int corner = 0; corner < 3; corner++)
						positionNormals[vertexPosition[mesh.Indices[i + corner]]] += faceNormal;
				}
				for (size_t i = 0; i < mesh.Vertices.size(); i++) {
					Vertex& vertex = mesh.Vertices[i];
					if (glm::dot(vertex.Normal, vertex.Normal) > 0.0f)
						continue; // came with a normal
					glm::vec3 n = positionNormals[vertexPosition[i]];
					vertex.Normal = glm::dot(n, n) > 0.0f ? glm::normalize(n) : glm::vec3(0, 1, 0);
				}
			}

			mesh.Name = std::filesystem::path(path).stem().string();
			mesh.ComputeBounds();
			return true;
		}

	}

	bool Load(const std::string& source, Mesh& mesh, std::string* error) {
		if (BuiltinMeshes::IsBuiltin(source)) {
			if (BuiltinMeshes::Create(source, mesh))
				return true;
			if (error)
				*error = "unknown built-in mesh " + source;
			return false;
		}

		std::string extension = std::filesystem::path(source).extension().string();
		for (char& c : extension)
			c = (char)tolower((unsigned char)c);

		Mesh loaded;
		loaded.Source = source;
		if (extension == ".obj") {
			if (!LoadOBJ(source, loaded, error))
				return false;
		}
		else {
			if (error)
				*error = source + ": unsupported mesh format '" + extension + "' (only .obj is supported)";
			return false;
		}

		mesh = std::move(loaded);
		return true;
	}

}
