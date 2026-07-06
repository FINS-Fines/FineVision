# ~/.fins/sdk/cmake/FineVision-extras.cmake

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