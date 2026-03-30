cmake_minimum_required(VERSION 3.28)

project(cyan LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(LLVM REQUIRED CONFIG)

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")

list(APPEND CMAKE_MODULE_PATH "${LLVM_CMAKE_DIR}")
include(AddLLVM)
include_directories(SYSTEM ${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})

llvm_map_components_to_libnames(
    LLVM_LIBS
    Passes
    Core
    MC
    Object
    Support
    TargetParser
    native
    nativecodegen
)

add_executable(
    cyan
    src/ast.cpp
    src/codegen.cpp
    src/diagnostic.cpp
    src/lexer.cpp
    src/lsp_support.cpp
    src/main.cpp
    src/parser.cpp
    src/sema_borrow.cpp
    src/sema_detail.cpp
    src/sema_expr.cpp
    src/sema_flow.cpp
    src/sema_stmt.cpp
    src/sema_support.cpp
    src/sema_types.cpp
    src/sema_views.cpp
    src/source.cpp
    src/type.cpp
)

target_include_directories(cyan PRIVATE include)
target_include_directories(cyan SYSTEM PRIVATE third_party/glaze/include)
target_link_libraries(cyan PRIVATE ${LLVM_LIBS})

add_executable(
    cyan_lsp_support_tests
    src/ast.cpp
    src/diagnostic.cpp
    src/lexer.cpp
    src/lsp_support.cpp
    src/parser.cpp
    src/sema_borrow.cpp
    src/sema_detail.cpp
    src/sema_expr.cpp
    src/sema_flow.cpp
    src/sema_stmt.cpp
    src/sema_support.cpp
    src/sema_types.cpp
    src/sema_views.cpp
    src/source.cpp
    src/type.cpp
    tests/cyan_lsp_support_tests.cpp
)

target_include_directories(cyan_lsp_support_tests PRIVATE include)
target_include_directories(
    cyan_lsp_support_tests
    SYSTEM PRIVATE
    third_party/glaze/include
)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|AppleClang")
    target_compile_options(
        cyan
        PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
        -Wshadow
        -Wimplicit-fallthrough
    )
    target_compile_options(
        cyan_lsp_support_tests
        PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
        -Wshadow
        -Wimplicit-fallthrough
    )
endif()

enable_testing()

add_test(
    NAME cyan_core_tests
    COMMAND ${CMAKE_COMMAND} -E env PYTHONPATH=${CMAKE_CURRENT_SOURCE_DIR}
            python3 ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_tests.py
            $<TARGET_FILE:cyan>
            ${CMAKE_CURRENT_SOURCE_DIR}/tests
)

add_test(
    NAME cyan_lsp_support_tests
    COMMAND $<TARGET_FILE:cyan_lsp_support_tests>
)
