// 写文件（spec 8.3、10）：Media Foundation IMFSinkWriter（MP4 = H.264 + AAC、M4A = AAC、MP3），以及自己写的 WAV。
// 所有方法只在写入线程上调用（该线程已 CoInitializeEx 与 mf::Startup）。
#pragma once

#include <windows.h>

#include <string>

struct IMFSinkWriter;
struct IMFSample;

namespace ls {
namespace rec {

class MediaWriter {
public:
    enum AudioCodec { kNoAudio = 0, kAac, kMp3 };
    struct VideoSpec {
        int width = 0, height = 0, fps = 30, bitrate = 0;
        bool bt709 = true;
    };
    ~MediaWriter();
    // video 为空表示纯声音（M4A/MP3）。hardware：优先硬件编码；失败时调用方用 false 再试一次。
    bool Open(const std::wstring &path, const VideoSpec *video, AudioCodec audio, bool hardware);
    bool HasVideo() const { return hasVideo_; }
    bool HasAudio() const { return hasAudio_; }
    // 视频样本（NV12，时间与时长已设好）
    bool WriteVideo(IMFSample *sample);
    // 16 位交错立体声 48 kHz；time 为 100 纳秒
    bool WriteAudio(const int16_t *pcm, int frames, int64_t time);
    // 编码器积压的视频帧数（收到 - 已编码）
    int VideoBacklog();
    bool Finish();  // Finalize；之后对象只能析构
    void Close();   // 不收尾直接释放（取消录制）

private:
    IMFSinkWriter *writer_ = nullptr;
    unsigned long videoStream_ = 0, audioStream_ = 0;
    bool hasVideo_ = false, hasAudio_ = false;
    bool began_ = false;
};

class WavWriter {
public:
    ~WavWriter();
    bool Open(const std::wstring &path);
    bool Write(const int16_t *pcm, int frames);
    bool Finish();
    void Close();

private:
    HANDLE file_ = INVALID_HANDLE_VALUE;
    unsigned long long bytes_ = 0;
    bool failed_ = false;
};

}  // namespace rec
}  // namespace ls
