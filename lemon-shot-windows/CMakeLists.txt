cmake_minimum_required(VERSION 3.16)
project(LemonShot LANGUAGES C CXX RC)

# Lemon Shot Windows 版：纯 Win32 + Direct2D/DirectWrite，C++17，单个 exe，静态链接 libc++。
# 支持 Windows 7 SP1 起全部桌面与服务器版本；x86/x64 用 msvcrt，ARM64 用 UCRT。
# 规则：任何 Win7 SP1 之后才有的 API 都必须运行时 GetProcAddress 取，不能直接链接（scripts/check-win-imports.sh 会检查）。

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE MinSizeRel)
endif()

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../lemon-shot-core ${CMAKE_BINARY_DIR}/core)

file(GLOB_RECURSE LS_WIN_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
add_executable(LemonShot WIN32 ${LS_WIN_SOURCES} res/app.rc)
target_include_directories(LemonShot PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(LemonShot PRIVATE
  _WIN32_WINNT=0x0601 WINVER=0x0601 NTDDI_VERSION=0x06010000
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN COBJMACROS
  LS_RESOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../lemon-shot-core/resources")
target_compile_options(LemonShot PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
  -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections)
target_link_options(LemonShot PRIVATE -municode -static -Wl,--gc-sections
  $<$<CONFIG:MinSizeRel,Release>:-s>)
target_link_libraries(LemonShot PRIVATE lemonshot_core
  d2d1 dwrite windowscodecs ole32 oleaut32 uuid shell32 shlwapi user32 gdi32 comctl32 comdlg32
  dwmapi uxtheme winmm winhttp imm32 advapi32 version d3d11 dxgi)
set_target_properties(LemonShot PROPERTIES OUTPUT_NAME LemonShot)

# 第二期组件：OCR（ncnn + PP-OCRv5，模型以 RCDATA 编进 exe，见 res/app.rc）、二维码（zxing-cpp）、WebP（libwebp）
include(${CMAKE_CURRENT_SOURCE_DIR}/../lemon-shot-core/cmake/extras.cmake)
lemonshot_add_extras(LemonShot OCR QR WEBP)
# simpleomp.cpp 需要 pthread_once：用 InitOnceExecuteOnce 补上（llvm-mingw 不带 winpthreads）
target_compile_options(ncnn PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:SHELL:-include ${CMAKE_CURRENT_SOURCE_DIR}/cmake/ncnn_win32_once.h>")
# 模型文件变化时重新编资源
set_property(SOURCE res/app.rc APPEND PROPERTY OBJECT_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/../lemon-shot-core/third_party/ppocr/PP_OCRv5_mobile_det.ncnn.param
  ${CMAKE_CURRENT_SOURCE_DIR}/../lemon-shot-core/third_party/ppocr/PP_OCRv5_mobile_det.ncnn.bin
  ${CMAKE_CURRENT_SOURCE_DIR}/../lemon-shot-core/third_party/ppocr/PP_OCRv5_mobile_rec.ncnn.param
  ${CMAKE_CURRENT_SOURCE_DIR}/../lemon-shot-core/third_party/ppocr/PP_OCRv5_mobile_rec.ncnn.bin
  ${CMAKE_CURRENT_SOURCE_DIR}/../lemon-shot-core/third_party/ppocr/ppocrv5_dict.txt)
