# -*- cmake -*-
include(Prebuilt)

add_library( ll::websocketpp INTERFACE IMPORTED )

# MSVC does not advertise the selected C++ language level through
# __cplusplus unless /Zc:__cplusplus is enabled. WebSocket++ 0.8.2 relies on
# that macro to choose its std:: type-traits implementation, so make the
# C++11-and-newer standard-library support explicit for this target.
target_compile_definitions(ll::websocketpp INTERFACE _WEBSOCKETPP_CPP11_STL_)

use_system_binary( websocketpp )
use_prebuilt_binary(websocketpp)

# WebSocket++ 0.8.2 still uses Boost.Asio's io_service API. That API was
# removed in Boost 1.87, while Firestorm's prebuilt Boost is newer. Linden's
# WebSocket++ integration carries the upstream compatibility patch; apply its
# header-only portion after autobuild has staged the prebuilt package.
find_package(Git REQUIRED)

set(_websocketpp_patches
    "${CMAKE_CURRENT_LIST_DIR}/../vcpkg/ports/websocketpp/boost187.patch"
    "${CMAKE_CURRENT_LIST_DIR}/../vcpkg/ports/websocketpp/cpp23.patch")
get_filename_component(_websocketpp_repo_root "${CMAKE_SOURCE_DIR}" DIRECTORY)
file(RELATIVE_PATH _websocketpp_include_dir
    "${_websocketpp_repo_root}" "${AUTOBUILD_INSTALL_DIR}/include")

# CMake may be run more than once against the same build directory. Detect an
# already-patched package first so configuration remains idempotent.
foreach (_websocketpp_patch IN LISTS _websocketpp_patches)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" apply --reverse --check
                "--directory=${_websocketpp_include_dir}"
                "--include=**/websocketpp/**"
                "${_websocketpp_patch}"
    WORKING_DIRECTORY "${_websocketpp_repo_root}"
        RESULT_VARIABLE _websocketpp_patch_already_applied
        OUTPUT_QUIET
        ERROR_QUIET)

    if (NOT _websocketpp_patch_already_applied EQUAL 0)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" apply
                    "--directory=${_websocketpp_include_dir}"
                    "--include=**/websocketpp/**"
                    "${_websocketpp_patch}"
            WORKING_DIRECTORY "${_websocketpp_repo_root}"
            RESULT_VARIABLE _websocketpp_patch_result
            OUTPUT_VARIABLE _websocketpp_patch_output
            ERROR_VARIABLE _websocketpp_patch_error)

        if (NOT _websocketpp_patch_result EQUAL 0)
            message(FATAL_ERROR
                "Could not apply the WebSocket++ Boost compatibility patch:\n"
                "${_websocketpp_patch_output}${_websocketpp_patch_error}")
        endif()
    endif()
endforeach()
