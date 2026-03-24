#pragma once
#include <windows.h>
#include <string>

// Shows a modal "Clone Git Repository" dialog.
// On success: clones the repository into destDir/<reponame>/ and returns that
//             absolute path so it can be fed to AddLibraryToCmake.
// On cancel or error: returns an empty string.
// Progress is shown inside the dialog; the call blocks until cloning finishes.
std::wstring ShowCloneDialog(HWND parent, const std::wstring &destDir);

// Same as ShowCloneDialog but pre-fills the URL and auto-starts the clone.
// Used by the Marketplace panel to show progress when installing a library.
std::wstring ShowInstallCloneDialog(HWND parent, const std::wstring &destDir, const std::wstring &url);
