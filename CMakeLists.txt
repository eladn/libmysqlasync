cmake_minimum_required(VERSION 2.8)

project(libmysqlasync C)

include_directories("${PROJECT_SOURCE_DIR}/include")

file(GLOB LIB_SOURCES "src/*.c")

set(GCC_COVERAGE_COMPILE_FLAGS "-std=gnu11")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${GCC_COVERAGE_COMPILE_FLAGS}")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${GCC_COVERAGE_COMPILE_FLAGS}")

add_library(mysqlasync STATIC ${LIB_SOURCES})

add_subdirectory(tests)

install(TARGETS mysqlasync DESTINATION lib)
install(DIRECTORY "${PROJECT_SOURCE_DIR}/include/" DESTINATION include/libmysqlasync)
