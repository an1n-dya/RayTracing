#include "FileDialogs.h"

#include "Walnut/Application.h"

#define NOMINMAX
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include <commdlg.h>

#pragma comment(lib, "comdlg32.lib")

namespace FileDialogs {

	static HWND GetOwnerWindow() {
		return glfwGetWin32Window(Walnut::Application::Get().GetWindowHandle());
	}

	std::string OpenFile(const char* filter) {
		CHAR fileName[MAX_PATH] = { 0 };

		OPENFILENAMEA ofn{};
		ofn.lStructSize = sizeof(OPENFILENAMEA);
		ofn.hwndOwner = GetOwnerWindow();
		ofn.lpstrFile = fileName;
		ofn.nMaxFile = sizeof(fileName);
		ofn.lpstrFilter = filter;
		ofn.nFilterIndex = 1;
		// NOCHANGEDIR: the shaders are loaded relative to the working directory
		ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;

		if (GetOpenFileNameA(&ofn) == TRUE)
			return ofn.lpstrFile;
		return {};
	}

	std::string SaveFile(const char* filter, const char* defaultExtension) {
		CHAR fileName[MAX_PATH] = { 0 };

		OPENFILENAMEA ofn{};
		ofn.lStructSize = sizeof(OPENFILENAMEA);
		ofn.hwndOwner = GetOwnerWindow();
		ofn.lpstrFile = fileName;
		ofn.nMaxFile = sizeof(fileName);
		ofn.lpstrFilter = filter;
		ofn.nFilterIndex = 1;
		ofn.lpstrDefExt = defaultExtension;
		ofn.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;

		if (GetSaveFileNameA(&ofn) == TRUE)
			return ofn.lpstrFile;
		return {};
	}

}
