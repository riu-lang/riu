
set(LIBS_DIR "${CMAKE_SOURCE_DIR}/third_party")
set(LIBS_BIN_DIR "${CMAKE_BINARY_DIR}/third_party")

add_subdirectory(antlr4)
add_subdirectory(llvm)
add_subdirectory(utfcpp)

set(LLVM_INCLUDE_DIR ${LLVM_INCLUDE_DIR} PARENT_SCOPE)
