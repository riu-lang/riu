project(yux_tests CXX)

add_subdirectory(../third_party/googletest ${CMAKE_BINARY_DIR}/third_party/googletest)

enable_testing()

add_executable(test_base
        test_base.cpp
)

target_compile_features(test_base PRIVATE cxx_std_20)

target_link_libraries(test_base
        gtest
        gtest_main
)

add_test(NAME test_base COMMAND test_base)
