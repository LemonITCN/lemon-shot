// 录制管线（spec 8.3、10）：采集线程（抓屏 → 缩放 → NV12 / GIF 帧）+ 写入线程（编码写文件、拉取混音），
// 声音来源各自一个线程（AudioSource）。界面线程只调用这里的方法，结果通过 PostMessage 通知。
//
// 线程模型：
//   界面线程   Prepare / Begin / Pause / Resume / RequestStop / Abort / Join
//   写入线程   CoInitializeEx(MTA) + mf::Startup → 建编码器、开声音设备 → 写帧、拉混音 → 收尾（Finalize / GIF 编码）
//   采集线程   CoInitializeEx(MTA) + timeBeginPeriod(1) → 按帧率抓屏，样本按时间线打时间戳放进有界队列（满了丢帧）
//   声音线程   每路一个，见 AudioCapture.h
// 停止时：时间线冻结 → 采集线程退出 → 写入线程写完队列、补齐声音、收尾 → WM_REC_DONE。全部资源在各自线程里释放。
#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "Record/AudioCapture.h"
#include "Record/RecordUtil.h"

struct IMFSample;

namespace ls {
namespace rec {

enum : UINT {
    WM_REC_READY = WM_APP + 0x300,  // wParam：1 就绪，0 失败；lParam：PipelineError
    WM_REC_DONE,                    // 收尾完成：wParam 1 成功
    WM_REC_PROGRESS,                // GIF 编码进度：wParam 千分比
    WM_REC_FAILED,                  // 录制中出错（写盘失败等），界面应停止
};

enum PipelineError { kErrNone = 0, kErrEncoder, kErrFile, kErrCapture, kErrNoFrames };

enum class OutputKind { Mp4, Gif, M4a, Mp3, Wav };

struct PipelineConfig {
    OutputKind kind = OutputKind::Mp4;
    // 画面（Mp4 / Gif）
    HMONITOR monitor = nullptr;
    RECT region = {};       // 屏幕像素
    bool cursor = true;
    int fps = 30;           // 采集帧率（GIF 为 gifFps）
    int outW = 0, outH = 0; // 输出像素尺寸（偶数）或 GIF 尺寸
    int bitrate = 0;
    // 声音
    bool mic = false;
    std::string micDevice;
    bool system = false;
    // 输出文件（最终位置）
    std::wstring path;
};

class Pipeline {
public:
    Pipeline() = default;
    ~Pipeline();
    Pipeline(const Pipeline &) = delete;
    Pipeline &operator=(const Pipeline &) = delete;

    // 启动线程并异步准备；就绪或失败时向 notify 发 WM_REC_READY
    bool Prepare(const PipelineConfig &cfg, HWND notify);
    Timeline &Clock() { return timeline_; }
    void Begin();  // 倒计时结束，时间线从现在开始
    void Pause();
    void Resume();
    // 停止并收尾；完成后发 WM_REC_DONE
    void RequestStop();
    // 取消：停掉全部线程并删除输出文件（同步，返回时线程都已结束）
    void Abort();
    // WM_REC_DONE 之后调用：等线程退出（很快）
    void Join();

    double Seconds();         // 当前有效录制时长
    double DurationSeconds(); // 停止后的总时长
    bool MicFailed() const { return micFailed_; }
    bool SystemFailed() const { return sysFailed_; }
    // 自上次读取以来的电平峰值（0..1，各路取最大）
    float TakeLevel();
    const PipelineConfig &Config() const { return cfg_; }

private:
    struct Frame {
        IMFSample *sample = nullptr;
        std::vector<uint8_t> pixels;
        int64_t ts = 0;
    };
    static DWORD WINAPI WriterProc(void *self);
    static DWORD WINAPI CaptureProc(void *self);
    void WriterRun();
    void CaptureRun();
    bool OpenOutput();
    void OpenAudio();
    void CloseAudio();
    void PumpAudio(int64_t upTo);
    bool WriteFrames(bool all);
    bool EncodeGif();
    void Post(UINT msg, WPARAM wp, LPARAM lp);
    bool HasVideo() const { return cfg_.kind == OutputKind::Mp4 || cfg_.kind == OutputKind::Gif; }
    bool WantsAudio() const { return cfg_.kind != OutputKind::Gif && (cfg_.mic || cfg_.system); }

    PipelineConfig cfg_;
    HWND notify_ = nullptr;
    Timeline timeline_;
    Thread writer_, capture_;
    Event stop_{true}, abort_{true}, frameReady_{false}, captureReady_{true}, captureDone_{true};
    volatile LONG captureOk_ = 0;
    volatile LONG failedPosted_ = 0;

    Lock qlock_;
    std::vector<Frame *> queue_;
    int dropped_ = 0;

    // 写入线程
    void *media_ = nullptr;  // MediaWriter
    void *wav_ = nullptr;    // WavWriter
    HANDLE gifTemp_ = INVALID_HANDLE_VALUE;
    std::wstring gifTempPath_;
    int gifFrames_ = 0;
    AudioMixer *mixer_ = nullptr;
    AudioSource *sources_[2] = {nullptr, nullptr};
    bool micFailed_ = false, sysFailed_ = false;
    std::vector<int16_t> pcm_;
    bool ok_ = true;
};

// 输出尺寸与码率（spec 8.3）
void VideoSize(int pxW, int pxH, const std::string &quality, int *outW, int *outH);
int VideoBitrate(int w, int h, int fps);

}  // namespace rec
}  // namespace ls
