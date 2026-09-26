#pragma once
// macOS AutoCloud root resolver.
//
// Native macOS scan paths are selected in autocloud_scan.cpp while keeping
// Steam's existing Windows-root tokens intact for cloud metadata compatibility.

#ifndef _WIN32

#include <string>
#include <cctype>

namespace AutoCloudPathResolver {

inline std::string WindowsRootToLinux(const std::string& windowsRoot) {
    std::string lower;
    for (char c : windowsRoot)
        lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    if (lower == "winappdatalocallow" ||
        lower == "winappdatalocal" ||
        lower == "winappdataroaming" ||
        lower == "winmydocuments" ||
        lower == "winsavedgames" ||
        lower == "winprogramdata" ||
        lower == "windowshome") {
        return windowsRoot;
    }
    return {};
}

// macOS native games do not use a Proton prefix. Keep the shared API and
// return an empty Proton mapping.
inline std::string WindowsRootToProton(const std::string&) {
    return {};
}

} // namespace AutoCloudPathResolver

#endif // !_WIN32
