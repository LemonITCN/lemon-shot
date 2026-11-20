/*
 * ncnn 的 simpleomp.cpp 在 Windows 上也用 pthread_once，而 llvm-mingw 的 libc++ 走 Win32 线程、不带 winpthreads。
 * 这里用 InitOnceExecuteOnce（Vista 起都有）补一个 pthread_once，只强制包含进 ncnn 的源文件（见 cmake/extras.cmake）。
 */
#ifndef LS_NCNN_WIN32_ONCE_H
#define LS_NCNN_WIN32_ONCE_H
#if defined(_WIN32) && defined(__cplusplus)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

typedef INIT_ONCE pthread_once_t;
#ifndef PTHREAD_ONCE_INIT
#define PTHREAD_ONCE_INIT INIT_ONCE_STATIC_INIT
#endif

static BOOL CALLBACK ls_ncnn_once_thunk(PINIT_ONCE, PVOID fn, PVOID *) {
    reinterpret_cast<void (*)(void)>(fn)();
    return TRUE;
}

static inline int pthread_once(pthread_once_t *once, void (*fn)(void)) {
    return InitOnceExecuteOnce(once, ls_ncnn_once_thunk, reinterpret_cast<PVOID>(fn), nullptr) ? 0 : -1;
}
#endif
#endif
