#pragma once
#include <string>
#include <vector>

// Represents one library found in a CMakeLists.txt via add_subdirectory()
struct CmakeSubLib
{
    std::wstring name;      // basename of the subdirectory (display name)
    std::wstring subdirRel; // relative path as written in CMakeLists.txt
    std::wstring subdirAbs; // absolute path (resolved against project root)
};

// Result of parsing a project's CMakeLists.txt
struct CmakeProjectInfo
{
    std::wstring projectRoot;  // directory containing CMakeLists.txt
    std::wstring targetName;   // first add_executable / add_library target found
    std::vector<CmakeSubLib>    libraries;    // add_subdirectory entries (non-system)
    std::vector<std::wstring>   includeDirs;  // target_include_directories paths (raw)
};

// Parse the CMakeLists.txt at cmakePath.
// Returns an empty struct if the file cannot be read.
CmakeProjectInfo ParseCmakeLists(const std::wstring &cmakePath);

// Append a library block to the CMakeLists.txt.
// projectCmakePath  — path to the project CMakeLists.txt to modify
// projectTargetName — CMake target of the project (e.g. "MyApp")
// subdirRel         — relative path for add_subdirectory (e.g. "libs/mylib")
// libTargetName     — CMake target exported by the lib (e.g. "mylib")
// includeRel        — relative include path (may be empty if lib exports its own headers)
// Returns true on success.
bool AddLibraryToCmake(const std::wstring &projectCmakePath,
                       const std::wstring &projectTargetName,
                       const std::wstring &subdirRel,
                       const std::wstring &libTargetName,
                       const std::wstring &includeRel);

// Remove the Nebula-managed lines for a library from the project CMakeLists.txt.
// The operation is best-effort and intentionally keeps generic system links
// such as opengl32/ws2_32/Threads to avoid breaking other remaining libraries.
bool RemoveLibraryFromCmake(const std::wstring &projectCmakePath,
                            const std::wstring &subdirRel,
                            const std::wstring &libTargetName);
