# biopass-presence: walk-away lock and IR unlock for Wayland sessions.
# Optional (BIOPASS_PRESENCE) since it needs wayland-client and libsystemd.
pkg_check_modules(WAYLAND REQUIRED IMPORTED_TARGET wayland-client)
pkg_check_modules(SYSTEMD REQUIRED IMPORTED_TARGET libsystemd)
pkg_get_variable(WL_PROTOCOLS wayland-protocols pkgdatadir)
set(IDLE_XML "${WL_PROTOCOLS}/staging/ext-idle-notify/ext-idle-notify-v1.xml")
set(GEN "${CMAKE_CURRENT_BINARY_DIR}/gen")
file(MAKE_DIRECTORY "${GEN}")
add_custom_command(
  OUTPUT "${GEN}/ext-idle-notify-v1.h" "${GEN}/ext-idle-notify-v1.c"
  COMMAND wayland-scanner client-header "${IDLE_XML}" "${GEN}/ext-idle-notify-v1.h"
  COMMAND wayland-scanner private-code "${IDLE_XML}" "${GEN}/ext-idle-notify-v1.c"
  DEPENDS "${IDLE_XML}")

add_executable(biopass-presence
    presence.cc
    ir_verify.cc
    "${GEN}/ext-idle-notify-v1.c"
)
target_include_directories(biopass-presence PRIVATE
    "${GEN}"
    ${ONNXRUNTIME_INCLUDE_DIRS}
    ${FaceDetection_INCLUDE_DIRS}
    ${FaceAntiSpoof_INCLUDE_DIRS}
    ${FaceRecognition_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}/core
    ${CMAKE_SOURCE_DIR}/face
)
target_link_libraries(biopass-presence
    ${ONNXRUNTIME_LIB}
    biopass_core
    biopass_face
    spdlog::spdlog_header_only
    PkgConfig::WAYLAND
    PkgConfig::SYSTEMD
)
