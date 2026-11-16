file(INSTALL
    "${CMAKE_CURRENT_LIST_DIR}/serve.hpp"
    "${CMAKE_CURRENT_LIST_DIR}/serve_core.hpp"
    "${CMAKE_CURRENT_LIST_DIR}/serve_cli.hpp"
    "${CMAKE_CURRENT_LIST_DIR}/serve_auth.hpp"
    "${CMAKE_CURRENT_LIST_DIR}/serve_http.hpp"
    "${CMAKE_CURRENT_LIST_DIR}/serve_daemon.hpp"
    "${CMAKE_CURRENT_LIST_DIR}/serve_relay.hpp"
    DESTINATION "${CURRENT_PACKAGES_DIR}/include")
file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/slimr-serve-config.cmake" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/LICENSE" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}" RENAME copyright)
