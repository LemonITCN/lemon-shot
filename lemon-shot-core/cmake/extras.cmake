# 可选组件：OCR（ncnn + PP-OCRv5）、二维码（zxing-cpp）、WebP（libwebp）。
# 用法（在 add_subdirectory(lemon-shot-core) 之后）：
#   include(<lemon-shot-core>/cmake/extras.cmake)
#   lemonshot_add_extras(<可执行目标> OCR QR WEBP)
# 每个组件编成独立静态库，给 lemonshot_core 定义 LEMONSHOT_WITH_xxx（去掉 src/extras_optional.cpp 里的空实现）。
set(LEMONSHOT_CORE_DIR ${CMAKE_CURRENT_LIST_DIR}/..)

function(lemonshot_add_extras target)
  cmake_parse_arguments(A "OCR;QR;WEBP" "" "" ${ARGN})
  set(TP ${LEMONSHOT_CORE_DIR}/third_party)

  if(A_WEBP AND NOT TARGET lemonshot_webp)
    file(GLOB _webp ${TP}/libwebp/src/enc/*.c ${TP}/libwebp/src/dsp/*.c ${TP}/libwebp/src/utils/*.c ${TP}/libwebp/sharpyuv/*.c)
    add_library(lemonshot_webp STATIC ${_webp} ${LEMONSHOT_CORE_DIR}/extras/webp.cpp)
    target_include_directories(lemonshot_webp PRIVATE ${TP}/libwebp ${TP}/libwebp/src ${LEMONSHOT_CORE_DIR}/include)
    target_compile_options(lemonshot_webp PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti> -w)
    target_compile_definitions(lemonshot_core PRIVATE LEMONSHOT_WITH_WEBP)
  endif()
  if(A_WEBP)
    target_link_libraries(${target} PRIVATE lemonshot_webp)
  endif()

  if(A_QR AND NOT TARGET lemonshot_qr)
    set(ZXING_READERS ON CACHE BOOL "" FORCE)
    set(ZXING_WRITERS OFF CACHE STRING "" FORCE)
    set(ZXING_C_API OFF CACHE BOOL "" FORCE)
    set(ZXING_USE_BUNDLED_ZINT OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    add_subdirectory(${TP}/zxing-cpp/core ${CMAKE_BINARY_DIR}/zxing EXCLUDE_FROM_ALL)
    set_target_properties(ZXing PROPERTIES CXX_STANDARD 20)
    target_compile_options(ZXing PRIVATE -w)
    add_library(lemonshot_qr STATIC ${LEMONSHOT_CORE_DIR}/extras/barcode.cpp)
    target_include_directories(lemonshot_qr PRIVATE ${LEMONSHOT_CORE_DIR}/include)
    set_target_properties(lemonshot_qr PROPERTIES CXX_STANDARD 20)
    target_compile_options(lemonshot_qr PRIVATE -fexceptions)
    target_link_libraries(lemonshot_qr PUBLIC ZXing::ZXing)
    target_compile_definitions(lemonshot_core PRIVATE LEMONSHOT_WITH_QR)
  endif()
  if(A_QR)
    target_link_libraries(${target} PRIVATE lemonshot_qr)
  endif()

  if(A_OCR AND NOT TARGET lemonshot_ocr)
    foreach(o NCNN_VULKAN NCNN_BUILD_TOOLS NCNN_BUILD_EXAMPLES NCNN_BUILD_BENCHMARK NCNN_BUILD_TESTS NCNN_PYTHON NCNN_SHARED_LIB
              NCNN_INSTALL_SDK NCNN_PIXEL_DRAWING NCNN_PIXEL_AFFINE NCNN_PIXEL_ROTATE NCNN_C_API NCNN_PLATFORM_API NCNN_INT8 NCNN_BF16
              NCNN_BENCHMARK NCNN_ENABLE_LTO)
      set(${o} OFF CACHE BOOL "" FORCE)
    endforeach()
    foreach(o NCNN_DISABLE_EXCEPTION NCNN_DISABLE_RTTI NCNN_STRING NCNN_PIXEL NCNN_THREADS)
      set(${o} ON CACHE BOOL "" FORCE)
    endforeach()
    if(WIN32)
      # mingw 没有 libomp：用 ncnn 自带的极简 OpenMP 运行时
      set(NCNN_OPENMP ON CACHE BOOL "" FORCE)
      set(NCNN_SIMPLEOMP ON CACHE BOOL "" FORCE)
      if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)")
        # Windows ARM64 下 LLVM 给带内联汇编的大函数生成 SEH 展开信息会崩溃，改走 intrinsics 实现
        set(NCNN_GNU_INLINE_ASM OFF CACHE BOOL "" FORCE)
      endif()
    endif()
    # 体积：只编 PP-OCRv5 用到的算子及卷积等实现内部会创建的辅助算子，其余全部关掉
    set(_ncnn_keep input split concat reshape permute squeeze expanddims flatten crop slice padding packing cast noop dropout
        binaryop unaryop eltwise convolution convolutiondepthwise convolution1d convolutiondepthwise1d deconvolution
        deconvolutiondepthwise innerproduct gemm matmul multiheadattention sdpa layernorm softmax pooling pooling1d interp
        sigmoid swish hardswish hardsigmoid relu clip tanh mish elu gelu selu softplus prelu batchnorm scale bias reduction)
    file(STRINGS ${TP}/ncnn/src/CMakeLists.txt _ncnn_layer_lines REGEX "ncnn_add_layer\\(")
    foreach(_line ${_ncnn_layer_lines})
      string(REGEX MATCH "ncnn_add_layer\\(([A-Za-z0-9_]+)" _m "${_line}")
      string(TOLOWER "${CMAKE_MATCH_1}" _lname)
      list(FIND _ncnn_keep ${_lname} _idx)
      if(_idx EQUAL -1)
        set(WITH_LAYER_${_lname} OFF CACHE BOOL "" FORCE)
      endif()
    endforeach()
    # 体积：去掉 OCR 用不上的指令集版本（int8/bf16 专用与 AVX512、SVE 系列，以及单独的 FMA、F16C 版本——AVX2 版本已包含 FMA）；
    # x86 每个算子只编 SSE2、AVX、AVX2 三份，ARM64 保留 ARMv8.2 fp16
    foreach(o NCNN_FMA NCNN_F16C NCNN_AVX512 NCNN_AVX512VNNI NCNN_AVX512BF16 NCNN_AVX512FP16 NCNN_AVXVNNI NCNN_AVXVNNIINT8 NCNN_AVXVNNIINT16
              NCNN_AVXNECONVERT NCNN_XOP NCNN_FMA4 NCNN_ARM82DOT NCNN_ARM82FP16FML NCNN_ARM84BF16 NCNN_ARM84I8MM
              NCNN_ARM86SVE NCNN_ARM86SVE2 NCNN_ARM86SVEBF16 NCNN_ARM86SVEI8MM NCNN_ARM86SVEF32MM)
      set(${o} OFF CACHE BOOL "" FORCE)
    endforeach()
    add_subdirectory(${TP}/ncnn ${CMAKE_BINARY_DIR}/ncnn EXCLUDE_FROM_ALL)
    target_compile_options(ncnn PRIVATE -w)  # 第三方源码的警告不看
    if(WIN32)
      # simpleomp 在 mingw 上要用 pthread_once，llvm-mingw 不带 winpthreads：用 InitOnceExecuteOnce 补一个
      target_compile_options(ncnn PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:SHELL:-include ${LEMONSHOT_CORE_DIR}/cmake/ncnn_win32_once.h>")
    endif()
    add_library(lemonshot_ocr STATIC ${LEMONSHOT_CORE_DIR}/extras/ocr.cpp)
    target_include_directories(lemonshot_ocr PRIVATE ${LEMONSHOT_CORE_DIR}/include)
    target_link_libraries(lemonshot_ocr PUBLIC ncnn)
    target_compile_definitions(lemonshot_core PRIVATE LEMONSHOT_WITH_OCR)
  endif()
  if(A_OCR)
    target_link_libraries(${target} PRIVATE lemonshot_ocr)
  endif()
endfunction()
