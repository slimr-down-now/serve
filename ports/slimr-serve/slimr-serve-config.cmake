include(CMakeFindDependencyMacro)
find_dependency(slimr-json CONFIG)
find_dependency(slimr-reflect CONFIG)
find_dependency(slimr-bugsink CONFIG)
if(NOT TARGET slimr::serve)
    add_library(slimr::serve INTERFACE IMPORTED)
    set_target_properties(slimr::serve PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_LIST_DIR}/../../include"
        INTERFACE_COMPILE_FEATURES cxx_std_26
        INTERFACE_LINK_LIBRARIES "slimr::json;slimr::reflect;slimr::bugsink")
endif()

find_package(httplib CONFIG QUIET)
if(TARGET httplib::httplib AND NOT TARGET slimr::serve_http)
    add_library(slimr::serve_http INTERFACE IMPORTED)
    set_target_properties(slimr::serve_http PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_LIST_DIR}/../../include"
        INTERFACE_COMPILE_FEATURES cxx_std_26
        INTERFACE_LINK_LIBRARIES "slimr::serve;httplib::httplib")
endif()
