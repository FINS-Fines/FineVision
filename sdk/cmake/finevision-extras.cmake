# finevision-extras.cmake — FineVision SDK CMake helpers
#
#   find_package(finevision REQUIRED)
#   fins_add_node(my_node src/my_node.cpp)

# ── fins_add_node ────────────────────────────────────────────────────
# Create a FineVision node plugin (.so), set metadata, and install.
#
#   fins_add_node(target_name source1.cpp [source2.cpp ...])
#
# A .so should contain exactly ONE node (one EXPORT_NODE + one
# DEFINE_PLUGIN_ENTRY per translation unit).  For multiple nodes
# call fins_add_node separately for each.
#
macro(fins_add_node _target)
    add_library(${_target} SHARED ${ARGN})

    # Link SDK
    if(TARGET finevision::fins_sdk)
        target_link_libraries(${_target} PRIVATE finevision::fins_sdk)
    else()
        target_link_libraries(${_target} PRIVATE fins_sdk)
    endif()

    # System libs
    find_package(Threads REQUIRED)
    target_link_libraries(${_target} PRIVATE Threads::Threads ${CMAKE_DL_LIBS})

    # ROS 2
    find_package(rclcpp QUIET)
    if(rclcpp_FOUND)
        get_target_property(_rclcpp_inc rclcpp::rclcpp INTERFACE_INCLUDE_DIRECTORIES)
        if(_rclcpp_inc)
            target_include_directories(${_target} PRIVATE ${_rclcpp_inc})
        endif()
        target_link_libraries(${_target} PRIVATE rclcpp::rclcpp)
        message(STATUS "[FINS] rclcpp → ${_target}")
    endif()

    # Auto workspace name + PKG_NAME / PKG_SOURCE / FINS_NODE / FMT_HEADER_ONLY
    fins_add_node_metadata(${_target})

    install(TARGETS ${_target}
        ARCHIVE DESTINATION lib
        LIBRARY DESTINATION lib
        RUNTIME DESTINATION bin
    )
endmacro()

# ── fins_add_node_metadata (legacy) ──────────────────────────────────
# Kept for backwards compatibility.  Prefer fins_add_node for new code.
#
function(fins_add_node_metadata target_name)
    if(${ARGC} EQUAL 1)
        execute_process(
            COMMAND python3 -c "
import os, sys, yaml

current_dir = os.path.abspath(sys.argv[1])
config_path = os.path.expanduser('~/.fins/config.yaml')

workspace_name = 'colcon_ws'

if os.path.exists(config_path):
    try:
        with open(config_path, 'r') as f:
            cfg = yaml.safe_load(f)
        local_pkgs = cfg.get('local_packages', [])

        sorted_pkgs = sorted(local_pkgs, key=lambda x: len(x.get('path', '')), reverse=True)

        for pkg in sorted_pkgs:
            pkg_path = os.path.abspath(os.path.expanduser(pkg.get('path', '')))
            if current_dir.startswith(pkg_path):
                workspace_name = pkg.get('name', 'colcon_ws')
                break
    except Exception:
        pass

print(workspace_name)
" "${CMAKE_CURRENT_SOURCE_DIR}"
            OUTPUT_VARIABLE calculated_ws
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        set(pkg_source ${calculated_ws})
    else()
        set(pkg_source ${ARGV1})
    endif()

    set_target_properties(${target_name} PROPERTIES
        OUTPUT_NAME "${target_name}"
        POSITION_INDEPENDENT_CODE ON
    )

    target_compile_definitions(${target_name} PRIVATE
        PKG_NAME="${target_name}"
        PKG_SOURCE="${pkg_source}"
        FMT_HEADER_ONLY
        FINS_NODE
    )

    message(STATUS "[FINS] --------------------------------------------------")
    message(STATUS "  Source code path: ${CMAKE_CURRENT_SOURCE_DIR}")
    message(STATUS "  Workspace name: ${pkg_source}")
    message(STATUS "  Node name: ${target_name}")
    message(STATUS "[FINS] --------------------------------------------------")
endfunction()