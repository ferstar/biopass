cmake_minimum_required(VERSION 3.22)
project(biopass_npu LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(RYZEN_AI_ROOT "" CACHE PATH "Extracted Ryzen AI 1.9.0 root")
set(STB_ROOT "" CACHE PATH "stb at revision 904aa67e1e2d1dec92959df63e700b166d5c1022")
if(NOT EXISTS "${RYZEN_AI_ROOT}/onnxruntime/lib/libonnxruntime.so.1.30.0")
  message(FATAL_ERROR "Set RYZEN_AI_ROOT to Ryzen AI 1.9.0")
endif()
if(NOT EXISTS "${STB_ROOT}/stb_image.h")
  message(FATAL_ERROR "Set STB_ROOT to the pinned stb source")
endif()
set(FACE "${CMAKE_CURRENT_SOURCE_DIR}/../face")
set(CORE "${CMAKE_CURRENT_SOURCE_DIR}/../core")
add_library(biopass_det SHARED
  "${FACE}/common/onnx_session.cc"
  "${FACE}/common/npu_config.cc"
  "${FACE}/detection/face_detection.cc"
  "${FACE}/detection/utils.cc"
  "${FACE}/resize.cc"
  "${FACE}/tensor.cc"
  "${FACE}/image_io.cc"
  "${FACE}/stb_impl.cc"
  "${CORE}/user_file.cc"
  "${CORE}/fs_user.cc"
  "${CORE}/user_paths.cc")
target_compile_definitions(biopass_det PRIVATE BIOPASS_VITISAI)
set(BIOPASS_NPU_TEST_CONFIG "" CACHE FILEPATH "User-owned fixture for smoke build only")
if(BIOPASS_NPU_TEST_CONFIG)
  target_compile_definitions(biopass_det PRIVATE
    BIOPASS_NPU_TEST_CONFIG="${BIOPASS_NPU_TEST_CONFIG}")
endif()
target_include_directories(biopass_det PUBLIC
  "${FACE}" "${FACE}/common" "${FACE}/detection" "${CORE}" "${STB_ROOT}"
  "${RYZEN_AI_ROOT}/onnxruntime/include/onnxruntime")
target_link_libraries(biopass_det PUBLIC
  "${RYZEN_AI_ROOT}/onnxruntime/lib/libonnxruntime.so.1.30.0")
# Installed recognition/anti-spoofing DSOs also export the old OnnxSession.
# Bind detector-internal calls locally so load order cannot select that copy.
target_link_options(biopass_det PRIVATE "-Wl,-Bsymbolic-functions")
set_target_properties(biopass_det PROPERTIES
  BUILD_WITH_INSTALL_RPATH TRUE
  INSTALL_RPATH "$ORIGIN;$ORIGIN/runtime/deployment/lib;/usr/lib/biopass;/opt/ryzen-ai/deployment/lib;/opt/ryzen-ai/xrt/lib;/opt/ryzen-ai/compat")
add_executable(npu-smoke smoke.cc)
target_link_libraries(npu-smoke PRIVATE biopass_det)
set_target_properties(npu-smoke PROPERTIES
  BUILD_WITH_INSTALL_RPATH TRUE INSTALL_RPATH "$ORIGIN")
