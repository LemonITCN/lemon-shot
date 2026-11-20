// 录屏/录音共用的小工具：QPC 时钟、录制时间线（扣除暂停）、临界区、事件、线程封装。
// 只用 Win32 原语（Win7 起都有），不用 std::thread/std::mutex。
#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace ls {
namespace rec {

// ---------------------------------------------------------------- 时钟（100 纳秒单位，与 WASAPI 的 QPC 位置同一时基）

int64_t Now100ns();
inline double ToSeconds(int64_t t100) { return (double)t100 / 1e7; }

// ---------------------------------------------------------------- 互斥

class Lock {
public:
    Lock() { InitializeCriticalSection(&cs_); }
    ~Lock() { DeleteCriticalSection(&cs_); }
    Lock(const Lock &) = delete;
    Lock &operator=(const Lock &) = delete;
    void Enter() { EnterCriticalSection(&cs_); }
    void Leave() { LeaveCriticalSection(&cs_); }

private:
    CRITICAL_SECTION cs_;
};

class Guard {
public:
    explicit Guard(Lock &l) : l_(l) { l_.Enter(); }
    ~Guard() { l_.Leave(); }
    Guard(const Guard &) = delete;
    Guard &operator=(const Guard &) = delete;

private:
    Lock &l_;
};

// 手动复位事件
class Event {
public:
    explicit Event(bool manual = true) { h_ = CreateEventW(nullptr, manual ? TRUE : FALSE, FALSE, nullptr); }
    ~Event() {
        if (h_) CloseHandle(h_);
    }
    Event(const Event &) = delete;
    Event &operator=(const Event &) = delete;
    void Set() { SetEvent(h_); }
    void Reset() { ResetEvent(h_); }
    bool Wait(DWORD ms) { return WaitForSingleObject(h_, ms) == WAIT_OBJECT_0; }
    bool IsSet() { return WaitForSingleObject(h_, 0) == WAIT_OBJECT_0; }
    HANDLE Handle() const { return h_; }

private:
    HANDLE h_ = nullptr;
};

// 线程：Start 传入静态函数与参数；Join 等待结束并关闭句柄
class Thread {
public:
    ~Thread() { Join(); }
    bool Start(DWORD(WINAPI *fn)(void *), void *arg);
    void Join();
    bool Running() const { return h_ != nullptr; }

private:
    HANDLE h_ = nullptr;
};

// ---------------------------------------------------------------- 录制时间线
// 开始后经过的「有效时间」：扣除暂停区间。所有采集线程按它给样本打时间戳，线程安全。
class Timeline {
public:
    void Start(int64_t now);
    void Pause(int64_t now);
    void Resume(int64_t now);
    void Stop(int64_t now);  // 冻结：之后 Elapsed 固定为停止时刻
    bool Started();
    bool Paused();
    bool Stopped();
    // now 时刻的有效时间（未开始为 0）
    int64_t Elapsed(int64_t now);
    // 把一个采样时刻换算到有效时间：落在暂停区间里、早于开始或晚于停止返回 false
    bool Map(int64_t t, int64_t *out);
    int64_t EndTime();  // 停止时的有效时间

private:
    struct Gap {
        int64_t from, to;  // 暂停区间（绝对时刻）；to = 0 表示仍在暂停
    };
    Lock lock_;
    bool started_ = false, stopped_ = false;
    int64_t start_ = 0, stop_ = 0;
    std::vector<Gap> gaps_;
    int64_t ElapsedLocked(int64_t now);
};

// 计时文字：mm:ss，满一小时 h:mm:ss
std::wstring ClockText(double seconds);

}  // namespace rec
}  // namespace ls
