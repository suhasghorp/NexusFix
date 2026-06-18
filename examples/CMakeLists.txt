# examples/CMakeLists.txt
# NexusFIX Examples

# Simple FIX client example
add_executable(simple_client simple_client.cpp)
target_link_libraries(simple_client PRIVATE nexusfix)

# Quick start: full connect-send-receive flow
add_executable(nexusfix_quick_start quick_start.cpp)
target_link_libraries(nexusfix_quick_start PRIVATE nexusfix)

# Set output directory
set_target_properties(simple_client nexusfix_quick_start
    PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/examples
)
