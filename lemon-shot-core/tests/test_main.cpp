// 测试入口：lemonshot_core_tests <fixtures 目录> [用例名过滤]
#include "test.h"

#include <chrono>
#include <vector>

namespace lst {

namespace {
std::vector<Case> &cases() {
    static std::vector<Case> v;
    return v;
}
int g_checks = 0;
int g_failures = 0;
bool g_case_failed = false;
std::string g_fixtures;
}  // namespace

void register_case(const char *name, const char *file, void (*fn)()) { cases().push_back({name, file, fn}); }

void fail(const char *file, int line, const std::string &msg) {
    ++g_failures;
    g_case_failed = true;
    const char *base = std::strrchr(file, '/');
    std::printf("    失败 %s:%d  %s\n", base ? base + 1 : file, line, msg.c_str());
}

void count_check() { ++g_checks; }

const std::string &fixtures() { return g_fixtures; }

}  // namespace lst

int main(int argc, char **argv) {
    lst::g_fixtures = argc > 1 ? argv[1] : "tests/fixtures";
    const char *filter = argc > 2 ? argv[2] : nullptr;
    int run = 0, passed = 0;
    for (const lst::Case &c : lst::cases()) {
        if (filter && !std::strstr(c.name, filter)) continue;
        lst::g_case_failed = false;
        const auto t0 = std::chrono::steady_clock::now();
        c.fn();
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        ++run;
        if (!lst::g_case_failed) ++passed;
        std::printf("%s %-40s %8.1f ms\n", lst::g_case_failed ? "[失败]" : "[通过]", c.name, ms);
    }
    std::printf("\n用例 %d，通过 %d，失败 %d；断言 %d，失败断言 %d\n", run, passed, run - passed, lst::g_checks, lst::g_failures);
    return lst::g_failures ? 1 : 0;
}
