set(WITH_STATIC_CRT OFF)
set(ANTLR_BUILD_CPP_TESTS OFF)
set(ANTLR4_INSTALL OFF)

add_subdirectory(${LIBS_DIR}/antlr4/runtime/Cpp ${LIBS_BIN_DIR}/antlr4)

set(antlr4_ROOT "${LIBS_DIR}/antlr4/runtime/Cpp" CACHE PATH "")
set(antlr4_INCLUDE_DIR_STATIC "${LIBS_DIR}/antlr4/runtime/Cpp/runtime/src" CACHE PATH "")

target_compile_definitions(
        antlr4_static
        PUBLIC
        ANTLR4CPP_STATIC
)

target_compile_options(antlr4_static PRIVATE -include chrono)
