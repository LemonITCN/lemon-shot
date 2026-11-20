#include "Record/AudioCapture.h"

#include <mmdeviceapi.h>
#include <audioclient.h>

#include <cmath>
#include <cstring>

#include "Util/Log.h"
#include "Util/Util.h"

namespace ls {
namespace rec {

// ---------------------------------------------------------------- 混音器

namespace {
const int64_t kTolerance = kAudioRate / 50;   // 20 ms 以内的时间戳抖动视为连续
const size_t kMaxBuffered = kAudioRate * 6;   // 单路最多缓存 6 秒（异常时丢最旧的）
}  // namespace

AudioMixer::AudioMixer(int sources) { src_.resize((size_t)(sources > 0 ? sources : 0)); }

void AudioMixer::Compact(Src &s) {
    if (s.head > (size_t)kAudioRate && s.head * 2 > s.buf.size() / 2) {
        s.buf.erase(s.buf.begin(), s.buf.begin() + (long)(s.head * 2));
        s.head = 0;
    }
}

void AudioMixer::Push(int index, int64_t pos, const float *data, int frames) {
    if (frames <= 0 || index < 0 || index >= (int)src_.size()) return;
    Guard g(lock_);
    Src &s = src_[(size_t)index];
    for (int i = 0; i < frames * 2; ++i) {
        float a = std::fabs(data[i]);
        if (a > s.peak) s.peak = a;
    }
    // 已经输出过的部分丢掉
    if (pos + frames <= pulled_) return;
    if (pos < pulled_) {
        int64_t skip = pulled_ - pos;
        data += skip * 2;
        frames -= (int)skip;
        pos = pulled_;
    }
    int64_t end = pulled_ + (int64_t)s.Avail();
    if (pos > end + kTolerance) {
        // 中间缺了一段（loopback 没有声音时系统不给数据）：补静音
        s.buf.insert(s.buf.end(), (size_t)(pos - end) * 2, 0.f);
    } else if (pos < end - kTolerance) {
        // 与已有数据重叠（设备时钟偏快）：丢掉重叠部分
        int64_t overlap = end - pos;
        if (overlap >= frames) return;
        data += overlap * 2;
        frames -= (int)overlap;
    }
    s.buf.insert(s.buf.end(), data, data + (size_t)frames * 2);
    if (s.Avail() > kMaxBuffered) s.head += s.Avail() - kMaxBuffered;
    Compact(s);
}

int AudioMixer::Pull(int64_t upTo, std::vector<int16_t> &out) {
    Guard g(lock_);
    int64_t n64 = upTo - pulled_;
    if (n64 <= 0) {
        out.clear();
        return 0;
    }
    if (n64 > kAudioRate * 2) n64 = kAudioRate * 2;  // 一次最多 2 秒
    int n = (int)n64;
    out.assign((size_t)n * 2, 0);
    std::vector<float> mix((size_t)n * 2, 0.f);
    for (auto &s : src_) {
        size_t take = s.Avail() < (size_t)n ? s.Avail() : (size_t)n;
        const float *p = s.buf.data() + s.head * 2;
        for (size_t i = 0; i < take * 2; ++i) mix[i] += p[i];
        s.head += take;
        Compact(s);
    }
    for (size_t i = 0; i < mix.size(); ++i) {
        float v = mix[i];
        if (v > 1.f) v = 1.f;
        if (v < -1.f) v = -1.f;
        out[i] = (int16_t)std::lrintf(v * 32767.f);
    }
    pulled_ += n;
    return n;
}

int64_t AudioMixer::Pulled() {
    Guard g(lock_);
    return pulled_;
}

float AudioMixer::TakePeak(int index) {
    if (index < 0 || index >= (int)src_.size()) return 0;
    Guard g(lock_);
    float p = src_[(size_t)index].peak;
    src_[(size_t)index].peak = 0;
    return p > 1.f ? 1.f : p;
}

// ---------------------------------------------------------------- 采集

namespace {

const GUID kSubFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
const GUID kSubPcm = {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

// 声道 → 左右增益（按 WAVEFORMATEXTENSIBLE 的声道掩码）
void SpeakerGains(DWORD speaker, float &l, float &r) {
    const float k = 0.7071f;
    switch (speaker) {
    case SPEAKER_FRONT_LEFT: l = 1; r = 0; break;
    case SPEAKER_FRONT_RIGHT: l = 0; r = 1; break;
    case SPEAKER_FRONT_CENTER: l = k; r = k; break;
    case SPEAKER_LOW_FREQUENCY: l = 0; r = 0; break;
    case SPEAKER_BACK_LEFT:
    case SPEAKER_SIDE_LEFT:
    case SPEAKER_FRONT_LEFT_OF_CENTER: l = k; r = 0; break;
    case SPEAKER_BACK_RIGHT:
    case SPEAKER_SIDE_RIGHT:
    case SPEAKER_FRONT_RIGHT_OF_CENTER: l = 0; r = k; break;
    default: l = 0.5f; r = 0.5f; break;
    }
}

}  // namespace

AudioSource::~AudioSource() { Stop(); }

bool AudioSource::Start(const std::string &deviceId, bool loopback, AudioMixer *mixer, int index, Timeline *timeline) {
    Stop();
    deviceId_ = deviceId;
    loopback_ = loopback;
    mixer_ = mixer;
    index_ = index;
    timeline_ = timeline;
    ok_ = false;
    stop_.Reset();
    ready_.Reset();
    if (!thread_.Start(&AudioSource::Proc, this)) return false;
    // 设备打开一般几十毫秒；超时视为失败但线程继续（之后成功也能用）
    ready_.Wait(3000);
    return ok_;
}

void AudioSource::Stop() {
    if (!thread_.Running()) return;
    stop_.Set();
    thread_.Join();
}

DWORD WINAPI AudioSource::Proc(void *self) {
    static_cast<AudioSource *>(self)->Run();
    return 0;
}

bool AudioSource::Open() {
    IMMDeviceEnumerator *en = (IMMDeviceEnumerator *)enumerator_;
    if (!en) return false;
    IMMDevice *dev = nullptr;
    HRESULT hr = E_FAIL;
    if (!deviceId_.empty()) {
        std::wstring id = Widen(deviceId_);
        hr = en->GetDevice(id.c_str(), &dev);
        if (FAILED(hr)) LS_LOG("record: 找不到声音设备，改用默认设备");
    }
    if (!dev) hr = en->GetDefaultAudioEndpoint(loopback_ ? eRender : eCapture, eConsole, &dev);
    if (FAILED(hr) || !dev) {
        LS_LOG("record: 没有%s设备 0x%08lx", loopback_ ? "播放" : "录音", (unsigned long)hr);
        return false;
    }
    IAudioClient *client = nullptr;
    hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void **)&client);
    dev->Release();
    if (FAILED(hr) || !client) {
        LS_LOG("record: 声音设备激活失败 0x%08lx", (unsigned long)hr);
        return false;
    }
    WAVEFORMATEX *wf = nullptr;
    hr = client->GetMixFormat(&wf);
    if (FAILED(hr) || !wf) {
        client->Release();
        return false;
    }
    rate_ = (int)wf->nSamplesPerSec;
    channels_ = wf->nChannels;
    bits_ = wf->wBitsPerSample;
    container_ = wf->nChannels ? wf->nBlockAlign / wf->nChannels : 0;
    float_ = wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
    DWORD mask = 0;
    if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE && wf->cbSize >= 22) {
        WAVEFORMATEXTENSIBLE *ext = (WAVEFORMATEXTENSIBLE *)wf;
        float_ = IsEqualGUID(ext->SubFormat, kSubFloat) != 0;
        if (!float_ && !IsEqualGUID(ext->SubFormat, kSubPcm)) container_ = 0;  // 不认识的格式
        mask = ext->dwChannelMask;
    } else if (wf->wFormatTag != WAVE_FORMAT_IEEE_FLOAT && wf->wFormatTag != WAVE_FORMAT_PCM) {
        container_ = 0;
    }
    if (float_ && container_ != 4) container_ = 0;
    // 声道增益
    gains_.assign((size_t)channels_ * 2, 0.f);
    if (channels_ == 1) {
        gains_[0] = gains_[1] = 1.f;
    } else if (channels_ == 2) {
        gains_[0] = 1.f;
        gains_[3] = 1.f;
    } else {
        static const DWORD order[] = {SPEAKER_FRONT_LEFT, SPEAKER_FRONT_RIGHT, SPEAKER_FRONT_CENTER, SPEAKER_LOW_FREQUENCY,
                                      SPEAKER_BACK_LEFT,  SPEAKER_BACK_RIGHT,  SPEAKER_SIDE_LEFT,    SPEAKER_SIDE_RIGHT};
        int ch = 0;
        for (DWORD bit = 1; bit && ch < channels_; bit <<= 1) {
            if (mask && !(mask & bit)) continue;
            DWORD sp = mask ? bit : (ch < 8 ? order[ch] : 0);
            float l, r;
            SpeakerGains(sp, l, r);
            gains_[(size_t)ch * 2] = l * 0.7f;
            gains_[(size_t)ch * 2 + 1] = r * 0.7f;
            ++ch;
            if (!mask && ch >= 8) break;
        }
    }
    // 共享模式，1 秒缓冲（我们每 10 ms 读一次，足够宽裕）
    DWORD flags = loopback_ ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0;
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 10000000, 0, wf, nullptr);
    CoTaskMemFree(wf);
    if (FAILED(hr)) {
        LS_LOG("record: IAudioClient::Initialize 失败 0x%08lx", (unsigned long)hr);
        client->Release();
        return false;
    }
    IAudioCaptureClient *cap = nullptr;
    hr = client->GetService(__uuidof(IAudioCaptureClient), (void **)&cap);
    if (FAILED(hr) || !cap) {
        client->Release();
        return false;
    }
    hr = client->Start();
    if (FAILED(hr)) {
        cap->Release();
        client->Release();
        return false;
    }
    client_ = client;
    capture_ = cap;
    rsIn_.assign(2, 0.f);  // 一帧历史
    rsPos_ = 1.0;
    LS_LOG("record: 声音设备已打开 %s %d Hz %d 声道 %d 位%s", loopback_ ? "loopback" : "录音", rate_, channels_, bits_,
           float_ ? " 浮点" : "");
    return container_ != 0 && rate_ > 0 && channels_ > 0;
}

void AudioSource::Close() {
    if (client_) ((IAudioClient *)client_)->Stop();
    if (capture_) ((IAudioCaptureClient *)capture_)->Release();
    if (client_) ((IAudioClient *)client_)->Release();
    capture_ = nullptr;
    client_ = nullptr;
}

void AudioSource::Run() {
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator *en = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void **)&en)))
        enumerator_ = en;
    ok_ = Open();
    if (!ok_) Close();
    ready_.Set();
    while (!stop_.Wait(10)) {
        if (!capture_) {
            // 设备失效后每秒尝试重新打开一次（例如换了默认播放设备、拔掉耳机）
            int64_t now = Now100ns();
            if (now - lastReopen_ > 10000000LL) {
                lastReopen_ = now;
                if (!Open()) Close();
            }
            continue;
        }
        ReadPackets();
    }
    Close();
    if (en) en->Release();
    enumerator_ = nullptr;
    if (SUCCEEDED(co)) CoUninitialize();
}

void AudioSource::ReadPackets() {
    IAudioCaptureClient *cap = (IAudioCaptureClient *)capture_;
    for (int guard = 0; guard < 200; ++guard) {
        UINT32 packet = 0;
        HRESULT hr = cap->GetNextPacketSize(&packet);
        if (FAILED(hr)) {
            LS_LOG("record: 声音设备失效 0x%08lx，稍后重新打开", (unsigned long)hr);
            Close();
            lastReopen_ = Now100ns();
            return;
        }
        if (packet == 0) return;
        BYTE *data = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;
        UINT64 devPos = 0, qpcPos = 0;
        hr = cap->GetBuffer(&data, &frames, &flags, &devPos, &qpcPos);
        if (hr == AUDCLNT_S_BUFFER_EMPTY) return;
        if (FAILED(hr)) {
            Close();
            lastReopen_ = Now100ns();
            return;
        }
        int64_t now = Now100ns();
        int64_t dur = rate_ > 0 ? (int64_t)frames * 10000000LL / rate_ : 0;
        int64_t t = (int64_t)qpcPos;
        // 没有可靠时间戳时按「现在减去这包的时长」估算
        if (t == 0 || (flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) || t > now + 20000000LL || t < now - 20000000LL)
            t = now - dur;
        Convert(data, frames, (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0, conv_);
        cap->ReleaseBuffer(frames);
        Resample(conv_, res_);
        int64_t active;
        if (!res_.empty() && timeline_ && timeline_->Map(t, &active))
            mixer_->Push(index_, FramesFromTime(active), res_.data(), (int)(res_.size() / 2));
    }
}

void AudioSource::Convert(const BYTE *data, UINT32 frames, bool silent, std::vector<float> &out) {
    out.assign((size_t)frames * 2, 0.f);
    if (silent || !data || container_ == 0) return;
    int ch = channels_;
    const float *g = gains_.data();
    for (UINT32 f = 0; f < frames; ++f) {
        float l = 0, r = 0;
        for (int c = 0; c < ch; ++c) {
            const BYTE *p = data + ((size_t)f * ch + c) * container_;
            float v;
            if (float_) {
                std::memcpy(&v, p, 4);
                if (!(v == v)) v = 0;  // NaN
            } else if (container_ == 2) {
                int16_t s;
                std::memcpy(&s, p, 2);
                v = s / 32768.f;
            } else if (container_ == 3) {
                int32_t s = (int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24);
                v = (float)(s / 2147483648.0);
            } else if (container_ == 4) {
                int32_t s;
                std::memcpy(&s, p, 4);
                v = (float)(s / 2147483648.0);
            } else {
                v = 0;
            }
            l += v * g[c * 2];
            r += v * g[c * 2 + 1];
        }
        out[(size_t)f * 2] = l;
        out[(size_t)f * 2 + 1] = r;
    }
}

// 三次 Hermite 插值重采样到 48 kHz；输入输出都是交错立体声
void AudioSource::Resample(const std::vector<float> &in, std::vector<float> &out) {
    out.clear();
    if (rate_ == kAudioRate) {
        out = in;
        return;
    }
    if (rate_ <= 0) return;
    rsIn_.insert(rsIn_.end(), in.begin(), in.end());
    size_t avail = rsIn_.size() / 2;
    double step = (double)rate_ / (double)kAudioRate;
    const float *s = rsIn_.data();
    // 需要 i-1、i、i+1、i+2 四帧
    while (true) {
        size_t i = (size_t)rsPos_;
        if (i + 2 >= avail) break;
        float t = (float)(rsPos_ - (double)i);
        for (int c = 0; c < 2; ++c) {
            float y0 = s[(i - 1) * 2 + c], y1 = s[i * 2 + c], y2 = s[(i + 1) * 2 + c], y3 = s[(i + 2) * 2 + c];
            float c1 = 0.5f * (y2 - y0);
            float c2 = y0 - 2.5f * y1 + 2.f * y2 - 0.5f * y3;
            float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
            out.push_back(((c3 * t + c2) * t + c1) * t + y1);
        }
        rsPos_ += step;
    }
    // 丢掉用完的输入，保留 i-1 起的历史
    size_t keepFrom = (size_t)rsPos_;
    keepFrom = keepFrom >= 1 ? keepFrom - 1 : 0;
    if (keepFrom > avail) keepFrom = avail;
    rsIn_.erase(rsIn_.begin(), rsIn_.begin() + (long)(keepFrom * 2));
    rsPos_ -= (double)keepFrom;
}

}  // namespace rec
}  // namespace ls
