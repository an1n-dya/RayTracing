-- premake5.lua
workspace "RayTracing"
   architecture "x64"
   configurations { "Debug", "Release", "Dist" }
   startproject "RayTracing"

outputdir = "%{cfg.buildcfg}-%{cfg.system}-%{cfg.architecture}"
include "Walnut/WalnutExternal.lua"

-- Walnut's Application.cpp is swapped for our copy in WalnutOverride/, which enables hardware ray tracing
-- (VK_KHR_ray_query) on the Vulkan device. The Walnut submodule itself stays untouched.
project "Walnut"
   removefiles { "Walnut/Walnut/src/Walnut/Application.cpp" }
   files { "WalnutOverride/Application.cpp", "WalnutOverride/WalnutExtensions.h" }
   includedirs { "WalnutOverride" }

include "RayTracing"