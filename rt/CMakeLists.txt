project(yux_rt CXX)

include_directories(
        "../include"
)

add_library(yux_rt STATIC
        std.cpp
)

add_custom_command(TARGET yux_rt POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:yux_rt> ${CMAKE_BINARY_DIR}
)