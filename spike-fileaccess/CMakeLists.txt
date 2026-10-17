cmake_minimum_required(VERSION 3.20)
project(swarm_spike LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(GODOTCPP_DISABLE_EXCEPTIONS ON)
add_subdirectory(godot-cpp)
add_library(swarm_spike SHARED src/register_types.cpp src/swarm_loader.cpp)
target_link_libraries(swarm_spike PRIVATE godot::cpp)
set_target_properties(swarm_spike PROPERTIES
    OUTPUT_NAME "swarm_spike"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/project/bin"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/project/bin"
    PREFIX "")
