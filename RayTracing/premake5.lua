project "RayTracing"
   kind "ConsoleApp"
   language "C++"
   cppdialect "C++17"
   targetdir "bin/%{cfg.buildcfg}"
   staticruntime "off"

   files { "src/**.h", "src/**.cpp", "src/**.comp" }

   includedirs
   {
      "../Walnut/vendor/imgui",
      "../Walnut/vendor/glfw/include",
      "../Walnut/vendor/glm",
      "../Walnut/vendor/stb_image",  -- stb_image is compiled into Walnut
      "../Walnut/vendor/glfw/deps",  -- stb_image_write.h (implementation compiled in ImageExport.cpp)

      "../Walnut/Walnut/src",

      "%{IncludeDir.VulkanSDK}",
   }

   links
   {
       "Walnut"
   }

   targetdir ("../bin/" .. outputdir .. "/%{prj.name}")
   objdir ("../bin-int/" .. outputdir .. "/%{prj.name}")

   -- Working directory for both F5 debugging and the built exe, so the GPU path tracer's
   -- relative shader path ("src/shaders/PathTrace.comp.spv") resolves the same way either way.
   debugdir "%{wks.location}/RayTracing"

   -- Compile the GPU compute shader to SPIR-V on every build (Vulkan SDK's glslc, already required to build this project).
   prebuildcommands {
      '"%{VULKAN_SDK}/Bin/glslc.exe" "%{wks.location}/RayTracing/src/shaders/PathTrace.comp" -o "%{wks.location}/RayTracing/src/shaders/PathTrace.comp.spv"'
   }

   filter "system:windows"
      systemversion "latest"
      defines { "WL_PLATFORM_WINDOWS" }

   filter "configurations:Debug"
      defines { "WL_DEBUG" }
      runtime "Debug"
      symbols "On"

   filter "configurations:Release"
      defines { "WL_RELEASE" }
      runtime "Release"
      optimize "On"
      symbols "On"

   filter "configurations:Dist"
      kind "WindowedApp"
      defines { "WL_DIST" }
      runtime "Release"
      optimize "On"
      symbols "Off"