cmake_minimum_required(VERSION 3.16)
project(lemonshot_core LANGUAGES C CXX)

# Lemon Shot 公共内核：静态库 + 单元测试。
# Mac 端由 SwiftPM 直接编译 src/（见 lemon-shot-macos/Package.swift），这里主要给 Windows 端和单元测试用。

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

file(GLOB LEMONSHOT_CORE_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
add_library(lemonshot_core STATIC ${LEMONSHOT_CORE_SOURCES})
target_include_directories(lemonshot_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(lemonshot_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
if(MSVC)
  target_compile_options(lemonshot_core PRIVATE /W4)
else()
  target_compile_options(lemonshot_core PRIVATE -Wall -Wextra -Wno-unused-parameter -fno-exceptions -fno-rtti)
endif()
if(WIN32)
  target_compile_definitions(lemonshot_core PRIVATE _WIN32_WINNT=0x0601 WINVER=0x0601 UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

option(LEMONSHOT_CORE_TESTS "Build core unit tests" OFF)
if(LEMONSHOT_CORE_TESTS)
  enable_testing()
  file(GLOB LEMONSHOT_CORE_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp)
  add_executable(lemonshot_core_tests ${LEMONSHOT_CORE_TEST_SOURCES})
  target_link_libraries(lemonshot_core_tests PRIVATE lemonshot_core)
  if(WIN32)
    # 测试程序也静态链接 libc++，拷到任何 Windows 上都能直接运行
    target_link_options(lemonshot_core_tests PRIVATE -static)
  endif()
  target_include_directories(lemonshot_core_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  add_test(NAME lemonshot_core_tests COMMAND lemonshot_core_tests ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures)
endif()
