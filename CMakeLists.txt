cmake_minimum_required(VERSION 3.8)
project(Nebula LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ========================================
# Configuration libvterm (neovim version)
# ========================================

set(LIBVTERM_DIR ${CMAKE_SOURCE_DIR}/external/libvterm)

# Vérifier que libvterm existe
if(NOT EXISTS ${LIBVTERM_DIR}/include/vterm.h)
  message(FATAL_ERROR "❌ libvterm not found. Clone it with:\n  git clone https://github.com/neovim/libvterm.git external/libvterm")
endif()

message(STATUS "✅ Using libvterm from: ${LIBVTERM_DIR}")

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

add_executable(Nebula WIN32 ${NEBULA_SOURCES})

target_compile_definitions(Nebula PRIVATE UNICODE _UNICODE)

if (MSVC)
  target_compile_options(Nebula PRIVATE /W4 /permissive-)
endif()

target_include_directories(Nebula PRIVATE src)
target_include_directories(Nebula PRIVATE external/nanosvg/src)

# ========================================
# Configuration ggwave
# ========================================

set(GGWAVE_DIR ${CMAKE_SOURCE_DIR}/external/ggwave)

# Vérifier que ggwave existe
if(NOT EXISTS ${GGWAVE_DIR}/include/ggwave/ggwave.h)
  message(FATAL_ERROR "❌ ggwave not found in ${GGWAVE_DIR}. Clone it with: git submodule update --init --recursive")
endif()

message(STATUS "✅ Using ggwave from: ${GGWAVE_DIR}")

# Ajouter ggwave comme sous-projet
add_subdirectory(${GGWAVE_DIR} EXCLUDE_FROM_ALL)

# ========================================
# Lier toutes les bibliothèques
# ========================================

# Lier libvterm
target_link_libraries(Nebula PRIVATE libvterm)

# Lier ggwave
target_link_libraries(Nebula PRIVATE ggwave)

# Bibliothèques Windows
target_link_libraries(Nebula PRIVATE 
  d3d11 
  dxgi 
  d2d1 
  dwrite 
  dwmapi 
  uxtheme 
  ole32
  winmm  # Pour waveOut
  dbghelp
)

if (MSVC)
  target_compile_definitions(Nebula PRIVATE "WIN32_LEAN_AND_MEAN")
endif()