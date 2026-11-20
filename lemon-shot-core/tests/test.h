// 极简单元测试框架：TEST 注册用例，CHECK 系列断言（失败不中断，继续跑后面的断言）。
#pragma once

#include "lemonshot.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace lst {

struct Case {
    const char *name;
    const char *file;
    void (*fn)();
};

void register_case(const char *name, const char *file, void (*fn)());
void fail(const char *file, int line, const std::string &msg);
void count_check();
const std::string &fixtures();

struct Reg {
    Reg(const char *name, const char *file, void (*fn)()) { register_case(name, file, fn); }
};

inline std::string fmt(double v) {
    char b[64];
    std::snprintf(b, sizeof b, "%.6g", v);
    return b;
}

}  // namespace lst

#define TEST(name)                                                       \
    static void test_##name();                                           \
    static lst::Reg reg_##name(#name, __FILE__, test_##name);            \
    static void test_##name()

#define CHECK(cond)                                                      \
    do {                                                                 \
        lst::count_check();                                              \
        if (!(cond)) lst::fail(__FILE__, __LINE__, "CHECK(" #cond ")"); \
    } while (0)

#define CHECK_EQ(a, b)                                                                                    \
    do {                                                                                                  \
        lst::count_check();                                                                               \
        const double va_ = (double)(a), vb_ = (double)(b);                                                \
        if (!(va_ == vb_))                                                                                \
            lst::fail(__FILE__, __LINE__, "CHECK_EQ(" #a ", " #b ") " + lst::fmt(va_) + " != " + lst::fmt(vb_)); \
    } while (0)

#define CHECK_NEAR(a, b, eps)                                                                                  \
    do {                                                                                                       \
        lst::count_check();                                                                                    \
        const double va_ = (double)(a), vb_ = (double)(b);                                                     \
        if (!(std::fabs(va_ - vb_) <= (double)(eps)))                                                          \
            lst::fail(__FILE__, __LINE__, "CHECK_NEAR(" #a ", " #b ") " + lst::fmt(va_) + " vs " + lst::fmt(vb_)); \
    } while (0)

#define CHECK_STR(a, b)                                                                                      \
    do {                                                                                                     \
        lst::count_check();                                                                                  \
        const std::string sa_ = (a), sb_ = (b);                                                              \
        if (sa_ != sb_) lst::fail(__FILE__, __LINE__, "CHECK_STR(" #a ", " #b ") \"" + sa_ + "\" != \"" + sb_ + "\""); \
    } while (0)
