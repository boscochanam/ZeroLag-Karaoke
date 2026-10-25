cmake_minimum_required(VERSION 3.20)

# Static CRT everywhere (including dependencies) so the exe runs standalone.
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project(LowLatencyKaraoke LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

include(FetchContent)

# ---- RtAudio (ASIO + WASAPI + DirectSound) ---------------------------------
set(RTAUDIO_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(RTAUDIO_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(RTAUDIO_BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(RTAUDIO_API_ASIO ON CACHE BOOL "" FORCE)
set(RTAUDIO_API_WASAPI ON CACHE BOOL "" FORCE)
set(RTAUDIO_API_DS ON CACHE BOOL "" FORCE)
FetchContent_Declare(rtaudio
  GIT_REPOSITORY https://github.com/thestk/rtaudio.git
  GIT_TAG        6.0.1
  GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(rtaudio)

# ---- Dear ImGui (Win32 + DX11 backend) --------------------------------------
FetchContent_Declare(imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG        v1.91.5
  GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(imgui)

add_library(imgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)

# ---- App ---------------------------------------------------------------------
add_executable(LowLatencyKaraoke WIN32
  src/main.cpp
  src/AudioEngine.cpp
  src/WasapiExclusive.cpp)
target_link_libraries(LowLatencyKaraoke PRIVATE rtaudio imgui d3d11 dxgi avrt ole32)
target_compile_definitions(LowLatencyKaraoke PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)

if(MSVC)
  # Fast float math, full optimisation, whole-program optimisation.
  target_compile_options(LowLatencyKaraoke PRIVATE /W4 /permissive- /fp:fast $<$<CONFIG:Release>:/O2 /Oi /Ot /GL>)
  target_link_options(LowLatencyKaraoke PRIVATE $<$<CONFIG:Release>:/LTCG /OPT:REF /OPT:ICF>)
endif()
