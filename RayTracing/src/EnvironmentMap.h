#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// An equirectangular (latitude/longitude) HDR environment, plus the tables for importance sampling it by
// brightness. Row 0 is straight up (+Y); u wraps around the horizon.
// Lookup and sampling routines: EnvironmentSampling.h (CPU) and shaders/Environment.glsl (GPU).
class EnvironmentMap {
public:
	static constexpr const char* BuiltinSunSky = "builtin:sun-sky";

	// source: an image file stb_image can read (.hdr for real HDR; LDR images are linearized), or BuiltinSunSky
	static std::shared_ptr<EnvironmentMap> Load(const std::string& source, std::string* error = nullptr);

	const std::string& GetSource() const { return m_Source; }
	uint32_t GetWidth() const { return m_Width; }
	uint32_t GetHeight() const { return m_Height; }
	uint64_t GetVersion() const { return m_Version; } // unique per loaded map, so renderers know when to re-upload

	const std::vector<glm::vec4>& GetTexels() const { return m_Texels; } // linear RGB radiance, row-major
	// Importance sampling tables (piecewise constant over texels, weighted by luminance * sin(theta)):
	const std::vector<float>& GetMarginalCDF() const { return m_MarginalCDF; }       // per row
	const std::vector<float>& GetConditionalCDF() const { return m_ConditionalCDF; } // per texel, cumulative within its row
	const std::vector<float>& GetPdf() const { return m_Pdf; }                       // per texel, density over (u, v) in [0,1]^2
	bool IsSampleable() const { return m_Sampleable; } // false for an all-black map

private:
	void BuildSamplingTables();

	std::string m_Source;
	uint32_t m_Width = 0, m_Height = 0;
	uint64_t m_Version = 0;
	std::vector<glm::vec4> m_Texels;
	std::vector<float> m_MarginalCDF, m_ConditionalCDF, m_Pdf;
	bool m_Sampleable = false;
};
