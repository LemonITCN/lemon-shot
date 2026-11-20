// 声音采集（spec 8.3、10）：WASAPI 共享模式采集麦克风、loopback 采集系统声音（Vista 起都有）。
// 每个来源一个线程：读包 → 转浮点立体声 → 重采样到 48 kHz → 按 QPC 时间戳放到时间线上交给混音器。
// 混音器按时间线位置对齐各路（容差 20 ms，超出时补静音或丢弃重叠），写入线程按需取出相加后的 16 位立体声。
#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "Record/RecordUtil.h"

namespace ls {
namespace rec {

const int kAudioRate = 48000;
const int kAudioChannels = 2;

class AudioMixer {
public:
    explicit AudioMixer(int sources);
    int Sources() const { return (int)src_.size(); }
    // 采集线程：pos 为第一帧在时间线上的位置（48 kHz 帧）；stereo 为交错的浮点立体声
    void Push(int index, int64_t pos, const float *stereo, int frames);
    // 写入线程：取出 [已取位置, upTo) 的混音（16 位交错立体声）；返回帧数
    int Pull(int64_t upTo, std::vector<int16_t> &out);
    int64_t Pulled();
    // 自上次读取以来的峰值（0..1），界面电平条用
    float TakePeak(int index);

private:
    struct Src {
        std::vector<float> buf;  // 交错立体声，buf[head*2] 对应时间线位置 pulled_
        size_t head = 0;
        float peak = 0;
        size_t Avail() const { return buf.size() / 2 - head; }
    };
    void Compact(Src &s);
    Lock lock_;
    std::vector<Src> src_;
    int64_t pulled_ = 0;
};

// 时间（100 纳秒，时间线有效时间）↔ 48 kHz 帧位置
inline int64_t FramesFromTime(int64_t t100) { return t100 / 10000000LL * kAudioRate + (t100 % 10000000LL) * kAudioRate / 10000000LL; }
inline int64_t TimeFromFrames(int64_t frames) {
    return frames / kAudioRate * 10000000LL + (frames % kAudioRate) * 10000000LL / kAudioRate;
}

// 一路 WASAPI 采集
class AudioSource {
public:
    ~AudioSource();
    // deviceId 为空 = 默认设备；loopback = 采集默认（或指定）播放设备的声音。
    // 阻塞到设备打开（最多约 3 秒），返回是否成功。timeline 未开始前的样本丢弃。
    bool Start(const std::string &deviceId, bool loopback, AudioMixer *mixer, int index, Timeline *timeline);
    void Stop();

private:
    static DWORD WINAPI Proc(void *self);
    void Run();
    bool Open();
    void Close();
    void ReadPackets();
    void Convert(const BYTE *data, UINT32 frames, bool silent, std::vector<float> &out);
    void Resample(const std::vector<float> &in, std::vector<float> &out);

    std::string deviceId_;
    bool loopback_ = false;
    AudioMixer *mixer_ = nullptr;
    int index_ = 0;
    Timeline *timeline_ = nullptr;
    Thread thread_;
    Event stop_{true};
    Event ready_{true};
    bool ok_ = false;

    // 线程内
    void *enumerator_ = nullptr;  // IMMDeviceEnumerator
    void *client_ = nullptr;      // IAudioClient
    void *capture_ = nullptr;     // IAudioCaptureClient
    int rate_ = 0, channels_ = 0, bits_ = 0, container_ = 0;
    bool float_ = false;
    std::vector<float> gains_;  // 每个声道对左右的增益（2 × channels）
    // 重采样状态（三次 Hermite）
    std::vector<float> rsIn_;  // 未用完的输入（交错立体声），开头保留一帧历史
    double rsPos_ = 1.0;
    std::vector<float> conv_, res_;
    int64_t lastReopen_ = 0;
};

}  // namespace rec
}  // namespace ls
