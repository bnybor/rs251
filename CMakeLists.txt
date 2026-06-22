cmake_minimum_required(VERSION 3.16)

project(rs251 VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

add_library(rs251 STATIC src/rs251.c)
target_compile_options(rs251 PRIVATE -fno-builtin -ffreestanding)
target_include_directories(rs251 PUBLIC include)
target_compile_definitions(rs251 PRIVATE RS251_VERSION="${PROJECT_VERSION}")

include(CTest)
if(BUILD_TESTING)
    add_executable(rs251_test tests/test_rs251.c)
    target_link_libraries(rs251_test PRIVATE rs251)
    add_test(NAME rs251_test COMMAND rs251_test)
endif()
