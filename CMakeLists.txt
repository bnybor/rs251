cmake_minimum_required(VERSION 3.16)

project(rs251 VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Build all targets with -fPIC so the static lib can also be linked into shared libs.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(RS251_SOURCES
    src/rs251.c)

add_library(rs251_shared SHARED ${RS251_SOURCES})
add_library(rs251_static STATIC ${RS251_SOURCES})

foreach(target rs251_shared rs251_static)
    target_include_directories(${target} PUBLIC include PRIVATE src)
    target_compile_definitions(${target} PRIVATE RS251_VERSION="${PROJECT_VERSION}")
endforeach()

set_target_properties(rs251_shared PROPERTIES OUTPUT_NAME rs251)
set_target_properties(rs251_static PROPERTIES OUTPUT_NAME rs251_static)

include(CTest)
if(BUILD_TESTING)
    add_executable(rs251_test tests/test_rs251.c)
    target_link_libraries(rs251_test PRIVATE rs251_static)
    add_test(NAME rs251_test COMMAND rs251_test)
endif()
