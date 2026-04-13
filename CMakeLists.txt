cmake_minimum_required(VERSION 3.8)
project(Nebula LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(NEBULA_APP_VERSION "2.8.0" CACHE STRING "Nebula application version")

# ========================================
# Configuration libvterm (neovim version)
# ========================================

set(LIBVTERM_DIR ${CMAKE_SOURCE_DIR}/external/libvterm)

# Vérifier que libvterm existe
if(NOT EXISTS ${LIBVTERM_DIR}/include/vterm.h)
  message(FATAL_ERROR "libvterm not found. Clone it with:\n  git clone https://github.com/neovim/libvterm.git external/libvterm")
endif()

message(STATUS "Using libvterm from: ${LIBVTERM_DIR}")

# Sources de libvterm
file(GLOB LIBVTERM_SOURCES 
    ${LIBVTERM_DIR}/src/*.c
)

# Créer la bibliothèque statique libvterm
add_library(libvterm STATIC ${LIBVTERM_SOURCES})

target_include_directories(libvterm PUBLIC 
    ${LIBVTERM_DIR}/include
)

# Configuration spécifique pour Windows/MSVC
if(MSVC)
    target_compile_definitions(libvterm PRIVATE
        _CRT_SECURE_NO_WARNINGS
        VTERM_STATIC
    )
    # Désactiver certains warnings pour le code externe
    target_compile_options(libvterm PRIVATE 
        /wd4244  # conversion warnings
        /wd4267  # size_t conversion
        /wd4018  # signed/unsigned mismatch
        /wd4146  # unary minus applied to unsigned
    )
endif()

# ========================================
# Collecter les sources Nebula
# ========================================

file(GLOB_RECURSE NEBULA_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_SOURCE_DIR}/src/*.c"
  "${CMAKE_SOURCE_DIR}/src/*.cpp"
  "${CMAKE_SOURCE_DIR}/src/*.cc"
  "${CMAKE_SOURCE_DIR}/src/*.cxx"
  "${CMAKE_SOURCE_DIR}/src/*.rc"
)

# Exclude temporary backup/experiment files to avoid duplicate symbols.
list(FILTER NEBULA_SOURCES EXCLUDE REGEX ".*_new\\.(c|cc|cpp|cxx)$")

add_executable(Nebula WIN32 ${NEBULA_SOURCES})

target_compile_definitions(Nebula PRIVATE UNICODE _UNICODE)
target_compile_definitions(Nebula PRIVATE NEBULA_APP_VERSION="${NEBULA_APP_VERSION}")

if (MSVC)
  # /FS avoids PDB write contention when compiling many files in parallel.
  target_compile_options(Nebula PRIVATE /W4 /permissive- /FS /utf-8)
endif()

target_include_directories(Nebula PRIVATE src)
target_include_directories(Nebula PRIVATE external/nanosvg/src)

# ========================================
# Configuration ggwave
# ========================================

set(GGWAVE_DIR ${CMAKE_SOURCE_DIR}/external/ggwave)

# Vérifier que ggwave existe
if(NOT EXISTS ${GGWAVE_DIR}/include/ggwave/ggwave.h)
  message(FATAL_ERROR "ggwave not found in ${GGWAVE_DIR}. Clone it with: git submodule update --init --recursive")
endif()

message(STATUS "Using ggwave from: ${GGWAVE_DIR}")

# Ajouter ggwave comme sous-projet
add_subdirectory(${GGWAVE_DIR} EXCLUDE_FROM_ALL)

# ========================================
# Configuration libgit2
# ========================================

set(LIBGIT2_DIR ${CMAKE_SOURCE_DIR}/external/libgit2)

if(NOT EXISTS ${LIBGIT2_DIR}/include/git2.h)
  message(FATAL_ERROR "libgit2 not found in ${LIBGIT2_DIR}. Clone it with: git clone https://github.com/libgit2/libgit2.git external/libgit2")
endif()

message(STATUS "Using libgit2 from: ${LIBGIT2_DIR}")

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_CLI OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(BUILD_FUZZERS OFF CACHE BOOL "" FORCE)
set(USE_SSH OFF CACHE STRING "" FORCE)
set(USE_HTTPS WinHTTP CACHE STRING "" FORCE)
if(MSVC)
  set(STATIC_CRT OFF CACHE BOOL "" FORCE)
endif()

add_subdirectory(${LIBGIT2_DIR} EXCLUDE_FROM_ALL)

# ========================================
# Lier toutes les bibliothèques
# ========================================

# Lier libvterm
target_link_libraries(Nebula PRIVATE libvterm)

# Lier ggwave
target_link_libraries(Nebula PRIVATE ggwave)

# Lier libgit2
target_link_libraries(Nebula PRIVATE libgit2package)
target_include_directories(Nebula PRIVATE ${LIBGIT2_DIR}/include)
target_include_directories(Nebula PRIVATE ${CMAKE_BINARY_DIR}/external/libgit2/include)

# Bibliothèques Windows
target_link_libraries(Nebula PRIVATE 
  d3d11 
  dxgi 
  d2d1 
  dwrite 
  dwmapi 
  shell32
  uxtheme 
  ole32
  windowscodecs
  crypt32
  winhttp
  ws2_32
  comctl32
  winmm  # Pour waveOut
  dbghelp
)

if (MSVC)
  target_compile_definitions(Nebula PRIVATE "WIN32_LEAN_AND_MEAN")
endif()

if (WIN32)
  target_link_libraries(Nebula PRIVATE opengl32)
endif()
