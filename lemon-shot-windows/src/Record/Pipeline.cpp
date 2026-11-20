#include "Record/Pipeline.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mmsystem.h>

#include <cmath>
#include <cstring>

#include "Record/Convert.h"
#include "Record/MF.h"
#include "Record/ScreenGrab.h"
#include "Record/Writers.h"
#include "Util/Api.h"
#include "Util/Log.h"
#include "Util/Util.h"
#include "lemonshot.h"

namespace ls {
namespace rec {

namespace {

const size_t kMaxQueue = 6;
const int64_t kAudioLatency = kAudioRate * 3 / 10;  // 写入时落后 300 ms，等各路设备的包到齐

int Even(double v) {
    int i = (int)std::floor(v);
    i &= ~1;
    return i < 2 ? 2 : i;
}

}  // namespace

void VideoSize(int pw, int ph, const std::string &quality, int *outW, int *outH) {
    double lng = pw > ph ? pw : ph;
    double limit = quality == "ultra" ? 4096 : (quality == "standard" ? 1280 : 1920);
    double f = lng > limit ? limit / lng : 1.0;
    // Windows 7 自带的 H.264 编码器最大 1920×1088
    if (!api::IsWin8OrLater()) {
        double fw = 1920.0 / (pw * f), fh = 1088.0 / (ph * f);
        double g = fw < fh ? fw : fh;
        if (g < 1) f *= g;
    }
    *outW = Even(pw * f);
    *outH = Even(ph * f);
}

int VideoBitrate(int w, int h, int fps) {
    double b = (double)w * h * fps * 0.1;
    if (b < 500000) b = 500000;
    if (b > 2000000000.0) b = 2000000000.0;
    return (int)b;
}

Pipeline::~Pipeline() {
    Abort();
    delete mixer_;
    mixer_ = nullptr;
}

void Pipeline::Post(UINT msg, WPARAM wp, LPARAM lp) {
    if (notify_) PostMessageW(notify_, msg, wp, lp);
}

bool Pipeline::Prepare(const PipelineConfig &cfg, HWND notify) {
    cfg_ = cfg;
    notify_ = notify;
    if (WantsAudio()) mixer_ = new AudioMixer(2);
    if (HasVideo() && !capture_.Start(&Pipeline::CaptureProc, this)) return false;
    if (!HasVideo()) {
        captureOk_ = 1;
        captureReady_.Set();
        captureDone_.Set();
    }
    if (!writer_.Start(&Pipeline::WriterProc, this)) {
        abort_.Set();
        capture_.Join();
        return false;
    }
    return true;
}

void Pipeline::Begin() { timeline_.Start(Now100ns()); }
void Pipeline::Pause() { timeline_.Pause(Now100ns()); }
void Pipeline::Resume() { timeline_.Resume(Now100ns()); }

void Pipeline::RequestStop() {
    if (!timeline_.Started()) timeline_.Start(Now100ns());
    timeline_.Stop(Now100ns());
    stop_.Set();
    frameReady_.Set();
}

void Pipeline::Abort() {
    abort_.Set();
    stop_.Set();
    frameReady_.Set();
    capture_.Join();
    writer_.Join();
    // 线程都已退出：清掉没写的帧
    Guard g(qlock_);
    for (Frame *f : queue_) {
        if (f->sample) f->sample->Release();
        delete f;
    }
    queue_.clear();
}

void Pipeline::Join() {
    capture_.Join();
    writer_.Join();
}

double Pipeline::Seconds() { return ToSeconds(timeline_.Elapsed(Now100ns())); }
double Pipeline::DurationSeconds() { return ToSeconds(timeline_.EndTime()); }

float Pipeline::TakeLevel() {
    if (!mixer_) return 0;
    float a = mixer_->TakePeak(0), b = mixer_->TakePeak(1);
    return a > b ? a : b;
}

// ---------------------------------------------------------------- 采集线程

DWORD WINAPI Pipeline::CaptureProc(void *self) {
    static_cast<Pipeline *>(self)->CaptureRun();
    return 0;
}

void Pipeline::CaptureRun() {
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    timeBeginPeriod(1);
    ScreenGrabber grab;
    bool ok = grab.Open(cfg_.monitor, cfg_.region, cfg_.cursor);
    InterlockedExchange(&captureOk_, ok ? 1 : 0);
    captureReady_.Set();
    if (ok) {
        bool gif = cfg_.kind == OutputKind::Gif;
        int sw = grab.Width(), sh = grab.Height();
        int ow = cfg_.outW, oh = cfg_.outH;
        bool scale = gif || ow != sw || oh != sh;
        Scaler scaler;
        std::vector<uint8_t> scaled;
        if (scale) {
            scaler.Setup(sw, sh, ow, oh);
            scaled.resize((size_t)ow * oh * 4);
        }
        bool bt709 = oh >= 720;
        int fps = cfg_.fps > 0 ? cfg_.fps : 30;
        int64_t period = 10000000LL / fps;
        int64_t next = 0, lastTs = -1;
        HANDLE waits[2] = {stop_.Handle(), abort_.Handle()};
        std::vector<uint8_t> lastGif;
        while (WaitForMultipleObjects(2, waits, FALSE, 0) == WAIT_TIMEOUT) {
            int64_t now = Now100ns();
            if (!timeline_.Started()) {
                WaitForMultipleObjects(2, waits, FALSE, 5);
                continue;
            }
            if (timeline_.Stopped()) break;
            if (next == 0) next = now;
            if (now < next) {
                DWORD ms = (DWORD)((next - now) / 10000);
                WaitForMultipleObjects(2, waits, FALSE, ms ? ms : 1);
                continue;
            }
            next += period;
            if (now - next > period) next = now + period;  // 落后太多：跳过
            if (timeline_.Paused()) continue;
            grab.Grab();
            int64_t ts;
            if (!timeline_.Map(Now100ns(), &ts)) continue;
            if (lastTs < 0) ts = 0;  // 第一帧作为第 0 帧
            if (ts <= lastTs) ts = lastTs + 1;
            const uint8_t *src = grab.Pixels();
            int sstride = grab.Stride();
            if (scale) {
                scaler.Run(src, sstride, scaled.data(), ow * 4);
                src = scaled.data();
                sstride = ow * 4;
            }
            Frame *f = new Frame();
            f->ts = ts;
            if (gif) {
                size_t bytes = (size_t)ow * oh * 4;
                // 与上一帧相同就不存（GIF 编码时并进上一帧的时长）
                if (lastGif.size() == bytes && std::memcmp(lastGif.data(), src, bytes) == 0) {
                    delete f;
                    continue;
                }
                lastGif.assign(src, src + bytes);
                f->pixels = lastGif;
            } else {
                DWORD bytes = (DWORD)(ow * oh * 3 / 2);
                IMFMediaBuffer *buf = nullptr;
                IMFSample *s = nullptr;
                BYTE *p = nullptr;
                if (SUCCEEDED(mf::CreateMemoryBuffer(bytes, &buf)) && buf && SUCCEEDED(buf->Lock(&p, nullptr, nullptr)) && p) {
                    BgraToNv12(src, sstride, ow, oh, p, ow, p + (size_t)ow * oh, ow, bt709);
                    buf->Unlock();
                    buf->SetCurrentLength(bytes);
                    if (SUCCEEDED(mf::CreateSample(&s)) && s) {
                        s->AddBuffer(buf);
                        s->SetSampleTime(ts);
                        s->SetSampleDuration(period);
                    }
                }
                if (buf) buf->Release();
                if (!s) {
                    delete f;
                    continue;
                }
                f->sample = s;
            }
            lastTs = ts;
            bool pushed = false;
            {
                Guard g(qlock_);
                if (queue_.size() < kMaxQueue) {
                    queue_.push_back(f);
                    pushed = true;
                }
            }
            if (pushed) {
                frameReady_.Set();
            } else {
                // 编码跟不上：丢帧
                if (f->sample) f->sample->Release();
                delete f;
                if (++dropped_ % 30 == 1) LS_LOG("record: 编码跟不上，已丢 %d 帧", dropped_);
            }
        }
    }
    grab.Close();
    captureDone_.Set();
    frameReady_.Set();
    timeEndPeriod(1);
    if (SUCCEEDED(co)) CoUninitialize();
}

// ---------------------------------------------------------------- 写入线程

DWORD WINAPI Pipeline::WriterProc(void *self) {
    static_cast<Pipeline *>(self)->WriterRun();
    return 0;
}

bool Pipeline::OpenOutput() {
    switch (cfg_.kind) {
    case OutputKind::Mp4:
    case OutputKind::M4a:
    case OutputKind::Mp3: {
        MediaWriter *w = new MediaWriter();
        media_ = w;
        MediaWriter::AudioCodec codec = MediaWriter::kNoAudio;
        if (WantsAudio()) codec = cfg_.kind == OutputKind::Mp3 ? MediaWriter::kMp3 : MediaWriter::kAac;
        MediaWriter::VideoSpec v;
        const MediaWriter::VideoSpec *vp = nullptr;
        if (cfg_.kind == OutputKind::Mp4) {
            v.width = cfg_.outW;
            v.height = cfg_.outH;
            v.fps = cfg_.fps;
            v.bitrate = cfg_.bitrate;
            v.bt709 = cfg_.outH >= 720;
            vp = &v;
        }
        // 硬件编码优先，失败退软件编码
        if (w->Open(cfg_.path, vp, codec, vp != nullptr)) return true;
        if (vp && w->Open(cfg_.path, vp, codec, false)) return true;
        return false;
    }
    case OutputKind::Wav: {
        WavWriter *w = new WavWriter();
        wav_ = w;
        return w->Open(cfg_.path);
    }
    case OutputKind::Gif: {
        gifTempPath_ = PathJoin(TempDir(), FormatW(L"LemonShot-gif-%lu-%llu.raw", GetCurrentProcessId(),
                                                   (unsigned long long)Now100ns()));
        gifTemp_ = CreateFileW(gifTempPath_.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_TEMPORARY, nullptr);
        return gifTemp_ != INVALID_HANDLE_VALUE;
    }
    }
    return false;
}

void Pipeline::OpenAudio() {
    if (!WantsAudio() || !mixer_) return;
    if (cfg_.mic) {
        sources_[0] = new AudioSource();
        if (!sources_[0]->Start(cfg_.micDevice, false, mixer_, 0, &timeline_)) micFailed_ = true;
    }
    if (cfg_.system) {
        sources_[1] = new AudioSource();
        if (!sources_[1]->Start(std::string(), true, mixer_, 1, &timeline_)) sysFailed_ = true;
    }
}

void Pipeline::CloseAudio() {
    for (auto &s : sources_) {
        if (s) {
            s->Stop();
            delete s;
            s = nullptr;
        }
    }
}

void Pipeline::PumpAudio(int64_t upTo) {
    if (!mixer_) return;
    for (int guard = 0; guard < 1000; ++guard) {
        int64_t from = mixer_->Pulled();
        if (from >= upTo) return;
        int64_t to = upTo - from > kAudioRate / 10 ? from + kAudioRate / 10 : upTo;
        int n = mixer_->Pull(to, pcm_);
        if (n <= 0) return;
        bool ok = true;
        if (media_)
            ok = ((MediaWriter *)media_)->WriteAudio(pcm_.data(), n, TimeFromFrames(from));
        else if (wav_)
            ok = ((WavWriter *)wav_)->Write(pcm_.data(), n);
        if (!ok && ok_) {
            ok_ = false;
            if (InterlockedExchange(&failedPosted_, 1) == 0) Post(WM_REC_FAILED, 0, 0);
        }
    }
}

// 把队列里的帧写出去；all = false 时编码器积压过多就先停（下次再写）
bool Pipeline::WriteFrames(bool all) {
    for (;;) {
        Frame *f = nullptr;
        {
            Guard g(qlock_);
            if (queue_.empty()) return true;
            if (!all && media_ && ((MediaWriter *)media_)->VideoBacklog() > 8) return true;
            f = queue_.front();
            queue_.erase(queue_.begin());
        }
        bool ok = true;
        if (f->sample) {
            ok = ((MediaWriter *)media_)->WriteVideo(f->sample);
            f->sample->Release();
        } else if (gifTemp_ != INVALID_HANDLE_VALUE) {
            DWORD n = 0;
            ok = WriteFile(gifTemp_, &f->ts, 8, &n, nullptr) && n == 8;
            if (ok) ok = WriteFile(gifTemp_, f->pixels.data(), (DWORD)f->pixels.size(), &n, nullptr) &&
                         n == (DWORD)f->pixels.size();
            if (ok) ++gifFrames_;
        }
        delete f;
        if (!ok && ok_) {
            ok_ = false;
            if (InterlockedExchange(&failedPosted_, 1) == 0) Post(WM_REC_FAILED, 0, 0);
        }
    }
}

void Pipeline::WriterRun() {
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool needMf = cfg_.kind == OutputKind::Mp4 || cfg_.kind == OutputKind::M4a || cfg_.kind == OutputKind::Mp3;
    bool mfStarted = needMf && mf::Startup();
    int err = kErrNone;
    if (needMf && !mfStarted) err = kErrEncoder;
    if (!err && !OpenOutput()) err = cfg_.kind == OutputKind::Gif || cfg_.kind == OutputKind::Wav ? kErrFile : kErrEncoder;
    if (!err) OpenAudio();
    // 等采集线程把抓屏准备好
    HANDLE ready[2] = {captureReady_.Handle(), abort_.Handle()};
    WaitForMultipleObjects(2, ready, FALSE, 10000);
    if (!err && !captureOk_) err = kErrCapture;
    ok_ = err == kErrNone;
    bool aborted = abort_.IsSet();
    if (!aborted) Post(WM_REC_READY, ok_ ? 1 : 0, err);

    if (ok_ && !aborted) {
        HANDLE waits[3] = {frameReady_.Handle(), stop_.Handle(), abort_.Handle()};
        for (;;) {
            DWORD r = WaitForMultipleObjects(3, waits, FALSE, 20);
            if (abort_.IsSet()) {
                aborted = true;
                break;
            }
            (void)r;
            WriteFrames(false);
            if (mixer_) {
                int64_t up = FramesFromTime(timeline_.Elapsed(Now100ns())) - kAudioLatency;
                PumpAudio(up);
            }
            if (stop_.IsSet() && captureDone_.IsSet()) break;
        }
        if (!aborted) {
            WriteFrames(true);
            // 等最后一段声音从设备送到（最多 400 ms），然后补齐到结束时刻
            if (mixer_) {
                int64_t end = FramesFromTime(timeline_.EndTime());
                for (int i = 0; i < 20 && !abort_.IsSet(); ++i) {
                    PumpAudio(end - kAudioLatency + (int64_t)i * kAudioLatency / 20);
                    Sleep(20);
                }
                PumpAudio(end);
            }
        }
    }
    CloseAudio();

    bool success = false;
    if (aborted || !ok_) {
        if (media_) ((MediaWriter *)media_)->Close();
        if (wav_) ((WavWriter *)wav_)->Close();
        DeleteFileW(cfg_.path.c_str());
    } else if (media_) {
        // 一帧画面都没有写进去时 Finalize 会失败
        success = ((MediaWriter *)media_)->Finish() && ok_;
        if (!success) DeleteFileW(cfg_.path.c_str());
    } else if (wav_) {
        success = ((WavWriter *)wav_)->Finish();
        if (!success) DeleteFileW(cfg_.path.c_str());
    } else if (gifTemp_ != INVALID_HANDLE_VALUE) {
        success = EncodeGif();
        if (!success) DeleteFileW(cfg_.path.c_str());
    }
    delete (MediaWriter *)media_;
    delete (WavWriter *)wav_;
    media_ = nullptr;
    wav_ = nullptr;
    if (gifTemp_ != INVALID_HANDLE_VALUE) CloseHandle(gifTemp_);
    gifTemp_ = INVALID_HANDLE_VALUE;
    if (!gifTempPath_.empty()) DeleteFileW(gifTempPath_.c_str());
    if (mfStarted) mf::Shutdown();
    if (SUCCEEDED(co)) CoUninitialize();
    // 准备失败时界面已经收到 WM_REC_READY(0)，不再发 DONE
    if (!aborted && err == kErrNone) Post(WM_REC_DONE, success ? 1 : 0, 0);
}

bool Pipeline::EncodeGif() {
    int w = cfg_.outW, h = cfg_.outH;
    size_t bytes = (size_t)w * h * 4;
    if (gifFrames_ <= 0 || bytes == 0) return false;
    LARGE_INTEGER zero;
    zero.QuadPart = 0;
    if (!SetFilePointerEx(gifTemp_, zero, nullptr, FILE_BEGIN)) return false;
    ls_gif *g = ls_gif_create(Narrow(cfg_.path).c_str(), w, h, 0);
    if (!g) return false;
    std::vector<uint8_t> a(bytes), b(bytes);
    int64_t startA = 0;
    bool ok = true;
    int lastPermille = -1;
    auto add = [&](std::vector<uint8_t> &px, int64_t delay) {
        if (delay > 65535) delay = 65535;
        if (delay < 2) delay = 2;
        ls_image img = {px.data(), w, h, w * 4};
        if (ls_gif_add(g, &img, (int32_t)delay) != 0) ok = false;
    };
    for (int i = 0; i < gifFrames_ && ok; ++i) {
        if (abort_.IsSet()) {
            ok = false;
            break;
        }
        int64_t ts = 0;
        DWORD n = 0;
        if (!ReadFile(gifTemp_, &ts, 8, &n, nullptr) || n != 8 || !ReadFile(gifTemp_, b.data(), (DWORD)bytes, &n, nullptr) ||
            n != (DWORD)bytes) {
            ok = false;
            break;
        }
        int64_t cs = (ts + 50000) / 100000;  // 百分之一秒
        if (i == 0) {
            a.swap(b);
            startA = 0;
        } else if (cs - startA >= 2) {
            add(a, cs - startA);
            a.swap(b);
            startA = cs;
        } else {
            // 间隔不足 2 厘秒（GIF 最小可靠时长）：用新画面替换，起始时间不变
            a.swap(b);
        }
        int permille = (int)((int64_t)(i + 1) * 1000 / gifFrames_);
        if (permille / 10 != lastPermille / 10) {
            lastPermille = permille;
            Post(WM_REC_PROGRESS, (WPARAM)permille, 0);
        }
    }
    if (ok) {
        int64_t endCs = (timeline_.EndTime() + 50000) / 100000;
        add(a, endCs - startA);
    }
    bool finished = ok && ls_gif_finish(g) == 0;
    ls_gif_destroy(g);
    return finished;
}

}  // namespace rec
}  // namespace ls
