# examples/CMakeLists.txt

cmake_minimum_required(VERSION 3.16)

macro(add_example target_name source_file)
    add_executable(${target_name} ${source_file})

    set_target_properties(${target_name} PROPERTIES ENABLE_EXPORTS ON)
    target_link_libraries(${target_name} PRIVATE fins_sdk ${CMAKE_DL_LIBS})

    # Optional ROS2 linking
    find_package(rclcpp QUIET)
    if(rclcpp_FOUND)
        target_link_libraries(${target_name} PRIVATE rclcpp::rclcpp)
    endif()
endmacro()

if (ENABLE_STATIC_BUILD)
    add_example(static_agent static_agent.cpp)
else()
    add_example(agent        agent.cpp       )
endif()