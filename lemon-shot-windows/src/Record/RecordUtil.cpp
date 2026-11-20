#include "Record/RecordUtil.h"

#include "Util/Util.h"

namespace ls {
namespace rec {

int64_t Now100ns() {
    static const int64_t freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f.QuadPart > 0 ? (int64_t)f.QuadPart : (int64_t)1;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    // 拆成整秒与余数，避免乘 1e7 溢出
    int64_t sec = c.QuadPart / freq;
    int64_t rem = c.QuadPart % freq;
    return sec * 10000000LL + rem * 10000000LL / freq;
}

bool Thread::Start(DWORD(WINAPI *fn)(void *), void *arg) {
    Join();
    h_ = CreateThread(nullptr, 0, fn, arg, 0, nullptr);
    return h_ != nullptr;
}

void Thread::Join() {
    if (!h_) return;
    WaitForSingleObject(h_, INFINITE);
    CloseHandle(h_);
    h_ = nullptr;
}

// ---------------------------------------------------------------- Timeline

void Timeline::Start(int64_t now) {
    Guard g(lock_);
    started_ = true;
    stopped_ = false;
    start_ = now;
    stop_ = 0;
    gaps_.clear();
}

void Timeline::Pause(int64_t now) {
    Guard g(lock_);
    if (!started_ || stopped_) return;
    if (!gaps_.empty() && gaps_.back().to == 0) return;
    gaps_.push_back({now, 0});
}

void Timeline::Resume(int64_t now) {
    Guard g(lock_);
    if (gaps_.empty() || gaps_.back().to != 0) return;
    gaps_.back().to = now > gaps_.back().from ? now : gaps_.back().from;
}

void Timeline::Stop(int64_t now) {
    Guard g(lock_);
    if (!started_ || stopped_) return;
    // 暂停中停止：停止时刻就是暂停开始时刻
    if (!gaps_.empty() && gaps_.back().to == 0) gaps_.back().to = now;
    stop_ = now;
    stopped_ = true;
}

bool Timeline::Started() {
    Guard g(lock_);
    return started_;
}

bool Timeline::Paused() {
    Guard g(lock_);
    return !gaps_.empty() && gaps_.back().to == 0;
}

bool Timeline::Stopped() {
    Guard g(lock_);
    return stopped_;
}

int64_t Timeline::ElapsedLocked(int64_t now) {
    if (!started_) return 0;
    if (stopped_ && now > stop_) now = stop_;
    if (now < start_) return 0;
    int64_t paused = 0;
    for (auto &gp : gaps_) {
        int64_t to = gp.to == 0 ? now : gp.to;
        if (gp.from >= now) break;
        if (to > now) to = now;
        if (to > gp.from) paused += to - gp.from;
    }
    int64_t e = now - start_ - paused;
    return e < 0 ? 0 : e;
}

int64_t Timeline::Elapsed(int64_t now) {
    Guard g(lock_);
    return ElapsedLocked(now);
}

bool Timeline::Map(int64_t t, int64_t *out) {
    Guard g(lock_);
    if (!started_ || t < start_) return false;
    if (stopped_ && t >= stop_) return false;
    for (auto &gp : gaps_) {
        if (t >= gp.from && (gp.to == 0 || t < gp.to)) return false;
    }
    *out = ElapsedLocked(t);
    return true;
}

int64_t Timeline::EndTime() {
    Guard g(lock_);
    return stopped_ ? ElapsedLocked(stop_) : ElapsedLocked(Now100ns());
}

std::wstring ClockText(double seconds) {
    long t = seconds > 0 ? (long)seconds : 0;
    long h = t / 3600, m = (t / 60) % 60, s = t % 60;
    if (h > 0) return FormatW(L"%ld:%02ld:%02ld", h, m, s);
    return FormatW(L"%02ld:%02ld", m, s);
}

}  // namespace rec
}  // namespace ls
