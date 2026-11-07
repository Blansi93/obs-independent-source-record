cmake_minimum_required(VERSION 3.28)

project(obs-record-source VERSION 0.2.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------
# OBS sources (headers live in <obs-source>/libobs and <obs-source>/frontend/api)
set(OBS_SOURCE_DIR "$ENV{OBS_SOURCE_DIR}" CACHE PATH "Path to the OBS Studio source tree")
if(NOT OBS_SOURCE_DIR)
  set(OBS_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.deps/obs-studio-33.0.1" CACHE PATH
      "Path to the OBS Studio source tree" FORCE)
endif()

# Qt 6 development files (pre-built obs-deps Qt archive)
set(OBS_QT_DIR "$ENV{OBS_QT_DIR}" CACHE PATH "Path to Qt 6 development files")
if(NOT OBS_QT_DIR)
  set(OBS_QT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.deps/qt6" CACHE PATH
      "Path to Qt 6 development files" FORCE)
endif()

# Installed OBS Studio (used only for its DLLs)
set(OBS_INSTALL_DIR "$ENV{OBS_INSTALL_DIR}" CACHE PATH "Path to the installed OBS Studio")
if(NOT OBS_INSTALL_DIR)
  set(OBS_INSTALL_DIR "$ENV{ProgramFiles}/obs-studio" CACHE PATH
      "Path to the installed OBS Studio" FORCE)
endif()

# Import libraries generated from the installed OBS DLLs by scripts/build.ps1
set(OBS_IMPORT_LIB_DIR "${CMAKE_BINARY_DIR}/importlibs" CACHE PATH
    "Directory containing obs.lib and obs-frontend-api.lib")

if(NOT EXISTS "${OBS_SOURCE_DIR}/libobs/obs.h")
  message(FATAL_ERROR "OBS source tree not found at '${OBS_SOURCE_DIR}'. "
                      "Pass -DOBS_SOURCE_DIR=... or run scripts/build.ps1.")
endif()
if(NOT EXISTS "${OBS_QT_DIR}/lib/cmake/Qt6/Qt6Config.cmake")
  message(FATAL_ERROR "Qt 6 not found at '${OBS_QT_DIR}'. "
                      "Pass -DOBS_QT_DIR=... or run scripts/build.ps1.")
endif()

set(OBS_LIB "${OBS_IMPORT_LIB_DIR}/obs.lib")
set(OBS_FRONTEND_LIB "${OBS_IMPORT_LIB_DIR}/obs-frontend-api.lib")
if(NOT EXISTS "${OBS_LIB}" OR NOT EXISTS "${OBS_FRONTEND_LIB}")
  message(FATAL_ERROR "OBS import libraries not found in '${OBS_IMPORT_LIB_DIR}'. "
                      "Run scripts/build.ps1, which generates them from the installed OBS DLLs.")
endif()

# ---------------------------------------------------------------------------
# Generated OBS configuration header
# ---------------------------------------------------------------------------
set(OBS_DATA_PATH "data/obs-plugins/%module%")
set(OBS_PLUGIN_PATH "obs-plugins/64bit")
set(OBS_PLUGIN_DESTINATION "obs-plugins")
set(OBS_RELEASE_CANDIDATE 0)
set(OBS_BETA 0)
set(GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${GENERATED_DIR}")
configure_file("${OBS_SOURCE_DIR}/libobs/obsconfig.h.in"
               "${GENERATED_DIR}/obsconfig.h" @ONLY)

# ---------------------------------------------------------------------------
# Qt 6
# ---------------------------------------------------------------------------
list(APPEND CMAKE_PREFIX_PATH "${OBS_QT_DIR}")
find_package(Qt6 6.5 REQUIRED COMPONENTS Core Widgets)

# ---------------------------------------------------------------------------
# Plugin target
# ---------------------------------------------------------------------------
add_library(obs-record-source MODULE
  src/plugin-main.cpp
  src/record-controller.cpp
  src/record-controller.hpp
  src/record-source-dock.cpp
  src/record-source-dock.hpp
  src/encoder-properties-dialog.cpp
  src/encoder-properties-dialog.hpp
  src/encoder-utils.hpp
  src/encoder-utils.cpp
  src/localization.cpp
  src/localization.hpp
  src/logger.cpp
  src/logger.hpp
)

target_include_directories(obs-record-source PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${OBS_SOURCE_DIR}/libobs"
  "${OBS_SOURCE_DIR}/frontend/api"
  "${GENERATED_DIR}"
)

target_compile_definitions(obs-record-source PRIVATE
  UNICODE
  _UNICODE
  WIN32_LEAN_AND_MEAN
  NOMINMAX
  _CRT_SECURE_NO_WARNINGS
)

target_link_libraries(obs-record-source PRIVATE
  Qt6::Core
  Qt6::Widgets
  "${OBS_FRONTEND_LIB}"
  "${OBS_LIB}"
)

set_target_properties(obs-record-source PROPERTIES
  AUTOMOC ON
  PREFIX ""
  OUTPUT_NAME "obs-record-source"
  SUFFIX ".dll"
)

if(MSVC)
  target_compile_options(obs-record-source PRIVATE /W3 /permissive- /utf-8)
  # OBS and the pre-built Qt libraries use the dynamic CRT.
  set_property(TARGET obs-record-source PROPERTY
               MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

# ---------------------------------------------------------------------------
# Stage an OBS 33 plugin bundle:
#   stage/obs-record-source/obs-record-source.dll
#   stage/obs-record-source/data/manifest.json
#   stage/obs-record-source/data/locale/en-US.ini
#   stage/obs-record-source/data/locale/es-ES.ini
# ---------------------------------------------------------------------------
set(STAGE_ROOT "${CMAKE_CURRENT_BINARY_DIR}/stage")
set(STAGE_DIR "${STAGE_ROOT}/obs-record-source")

add_custom_command(TARGET obs-record-source POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${STAGE_DIR}/data/locale"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "$<TARGET_FILE:obs-record-source>" "${STAGE_DIR}/obs-record-source.dll"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "${CMAKE_CURRENT_SOURCE_DIR}/data/manifest.json" "${STAGE_DIR}/data/manifest.json"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "${CMAKE_CURRENT_SOURCE_DIR}/data/locale/en-US.ini" "${STAGE_DIR}/data/locale/en-US.ini"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "${CMAKE_CURRENT_SOURCE_DIR}/data/locale/es-ES.ini" "${STAGE_DIR}/data/locale/es-ES.ini"
  COMMENT "Staging OBS plugin bundle in ${STAGE_DIR}"
)

# Convenience install target (used by scripts/build.ps1 -Install).
install(TARGETS obs-record-source
        RUNTIME DESTINATION "obs-studio/plugins/obs-record-source"
        LIBRARY DESTINATION "obs-studio/plugins/obs-record-source")
install(FILES data/manifest.json DESTINATION "obs-studio/plugins/obs-record-source/data")
install(FILES data/locale/en-US.ini data/locale/es-ES.ini
        DESTINATION "obs-studio/plugins/obs-record-source/data/locale")
