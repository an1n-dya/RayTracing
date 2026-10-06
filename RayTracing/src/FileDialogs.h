#pragma once

#include <string>

// Native (Win32) open/save file dialogs, owned by the Walnut window.
// filter uses the Win32 format: pairs of "Description\0pattern;pattern\0", e.g. "PNG image (*.png)\0*.png\0".
// Both return an empty string if the dialog was cancelled.
namespace FileDialogs {

	std::string OpenFile(const char* filter);
	std::string SaveFile(const char* filter, const char* defaultExtension);

}
