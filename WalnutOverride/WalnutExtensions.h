#pragma once

// Additions to Walnut's Application, implemented in WalnutOverride/Application.cpp (our copy of Walnut's
// Application.cpp, swapped in by the root premake5.lua so the Walnut submodule itself stays untouched).
namespace WalnutExtensions {

	// True if the Vulkan device was created with VK_KHR_acceleration_structure + VK_KHR_ray_query and buffer device
	// addresses, i.e. compute shaders can use hardware ray tracing
	bool IsRayQuerySupported();

}
