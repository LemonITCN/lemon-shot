#include "Record/Writers.h"

#include <cstring>

#include "Record/AudioCapture.h"
#include "Record/MF.h"
#include "Util/Api.h"
#include "Util/ComPtr.h"
#include "Util/Log.h"

namespace ls {
namespace rec {

namespace {

inline UINT64 Pack(UINT32 hi, UINT32 lo) { return ((UINT64)hi << 32) | lo; }

HRESULT VideoTypes(const MediaWriter::VideoSpec &v, int profile, IMFMediaType **outType, IMFMediaType **inType) {
    ComPtr<IMFMediaType> out, in;
    HRESULT hr = mf::CreateMediaType(out.Put());
    if (FAILED(hr)) return hr;
    out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    out->SetUINT32(MF_MT_AVG_BITRATE, (UINT32)v.bitrate);
    out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    out->SetUINT64(MF_MT_FRAME_SIZE, Pack((UINT32)v.width, (UINT32)v.height));
    out->SetUINT64(MF_MT_FRAME_RATE, Pack((UINT32)v.fps, 1));
    out->SetUINT64(MF_MT_PIXEL_ASPECT_RATIO, Pack(1, 1));
    if (profile) out->SetUINT32(MF_MT_MPEG2_PROFILE, (UINT32)profile);
    hr = mf::CreateMediaType(in.Put());
    if (FAILED(hr)) return hr;
    in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    in->SetUINT64(MF_MT_FRAME_SIZE, Pack((UINT32)v.width, (UINT32)v.height));
    in->SetUINT64(MF_MT_FRAME_RATE, Pack((UINT32)v.fps, 1));
    in->SetUINT64(MF_MT_PIXEL_ASPECT_RATIO, Pack(1, 1));
    in->SetUINT32(MF_MT_DEFAULT_STRIDE, (UINT32)v.width);
    in->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE);
    in->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    in->SetUINT32(MF_MT_SAMPLE_SIZE, (UINT32)(v.width * v.height * 3 / 2));
    in->SetUINT32(MF_MT_YUV_MATRIX, v.bt709 ? MFVideoTransferMatrix_BT709 : MFVideoTransferMatrix_BT601);
    in->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
    *outType = out.Detach();
    *inType = in.Detach();
    return S_OK;
}

HRESULT AudioTypes(MediaWriter::AudioCodec codec, IMFMediaType **outType, IMFMediaType **inType) {
    ComPtr<IMFMediaType> out, in;
    HRESULT hr = mf::CreateMediaType(out.Put());
    if (FAILED(hr)) return hr;
    out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    out->SetGUID(MF_MT_SUBTYPE, codec == MediaWriter::kMp3 ? MFAudioFormat_MP3 : MFAudioFormat_AAC);
    out->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kAudioRate);
    out->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, kAudioChannels);
    out->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 24000);  // 192 kbps
    if (codec == MediaWriter::kMp3) {
        out->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 1);
    } else {
        out->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    }
    hr = mf::CreateMediaType(in.Put());
    if (FAILED(hr)) return hr;
    in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    in->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    in->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    in->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kAudioRate);
    in->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, kAudioChannels);
    in->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, kAudioChannels * 2);
    in->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, kAudioRate * kAudioChannels * 2);
    in->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    *outType = out.Detach();
    *inType = in.Detach();
    return S_OK;
}

}  // namespace

MediaWriter::~MediaWriter() { Close(); }

bool MediaWriter::Open(const std::wstring &path, const VideoSpec *video, AudioCodec audio, bool hardware) {
    Close();
    // H.264 档次：Win8+ 用 High，Win7 的编码器用 Main；不行再不指定
    int profiles[3] = {api::IsWin8OrLater() ? 100 /* High */ : 77 /* Main */, 77, 0};
    int tries = video ? 3 : 1;
    for (int attempt = 0; attempt < tries; ++attempt) {
        if (attempt == 1 && profiles[0] == 77) continue;
        ComPtr<IMFAttributes> attr;
        if (FAILED(mf::CreateAttributes(attr.Put(), 4))) return false;
        attr->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, hardware ? TRUE : FALSE);
        attr->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
        attr->SetGUID(MF_TRANSCODE_CONTAINERTYPE, audio == kMp3 && !video ? MFTranscodeContainerType_MP3
                                                                           : MFTranscodeContainerType_MPEG4);
        DeleteFileW(path.c_str());
        IMFSinkWriter *w = nullptr;
        HRESULT hr = mf::CreateSinkWriterFromURL(path.c_str(), attr.Get(), &w);
        if (FAILED(hr) || !w) {
            LS_LOG("record: 创建 SinkWriter 失败 0x%08lx", (unsigned long)hr);
            return false;
        }
        writer_ = w;
        bool ok = true;
        if (video) {
            ComPtr<IMFMediaType> out, in;
            hr = VideoTypes(*video, profiles[attempt], out.Put(), in.Put());
            if (SUCCEEDED(hr)) hr = writer_->AddStream(out.Get(), &videoStream_);
            if (SUCCEEDED(hr)) hr = writer_->SetInputMediaType(videoStream_, in.Get(), nullptr);
            if (FAILED(hr)) {
                LS_LOG("record: 视频编码器设置失败 0x%08lx（档次 %d，硬件 %d）", (unsigned long)hr, profiles[attempt],
                       hardware ? 1 : 0);
                ok = false;
            }
            hasVideo_ = ok;
        }
        if (ok && audio != kNoAudio) {
            ComPtr<IMFMediaType> out, in;
            hr = AudioTypes(audio, out.Put(), in.Put());
            if (SUCCEEDED(hr)) hr = writer_->AddStream(out.Get(), &audioStream_);
            if (SUCCEEDED(hr)) hr = writer_->SetInputMediaType(audioStream_, in.Get(), nullptr);
            if (FAILED(hr)) {
                LS_LOG("record: 音频编码器设置失败 0x%08lx", (unsigned long)hr);
                ok = false;
            }
            hasAudio_ = ok;
        }
        if (ok) {
            hr = writer_->BeginWriting();
            if (FAILED(hr)) {
                LS_LOG("record: BeginWriting 失败 0x%08lx", (unsigned long)hr);
                ok = false;
            }
        }
        if (ok) {
            began_ = true;
            LS_LOG("record: 编码器就绪 %ls（视频 %dx%d，硬件 %d）", path.c_str(), video ? video->width : 0,
                   video ? video->height : 0, hardware ? 1 : 0);
            return true;
        }
        bool videoFailed = video && !hasVideo_;
        Close();
        DeleteFileW(path.c_str());
        // 音频失败与档次无关，不再重试
        if (videoFailed && attempt + 1 < tries) continue;
        break;
    }
    return false;
}

bool MediaWriter::WriteVideo(IMFSample *sample) {
    if (!writer_ || !hasVideo_ || !sample) return false;
    HRESULT hr = writer_->WriteSample(videoStream_, sample);
    if (FAILED(hr)) LS_LOG("record: 写视频帧失败 0x%08lx", (unsigned long)hr);
    return SUCCEEDED(hr);
}

bool MediaWriter::WriteAudio(const int16_t *pcm, int frames, int64_t time) {
    if (!writer_ || !hasAudio_ || frames <= 0) return frames <= 0;
    DWORD bytes = (DWORD)frames * kAudioChannels * 2;
    ComPtr<IMFMediaBuffer> buf;
    if (FAILED(mf::CreateMemoryBuffer(bytes, buf.Put()))) return false;
    BYTE *p = nullptr;
    if (FAILED(buf->Lock(&p, nullptr, nullptr)) || !p) return false;
    std::memcpy(p, pcm, bytes);
    buf->Unlock();
    buf->SetCurrentLength(bytes);
    ComPtr<IMFSample> s;
    if (FAILED(mf::CreateSample(s.Put()))) return false;
    s->AddBuffer(buf.Get());
    s->SetSampleTime(time);
    s->SetSampleDuration(TimeFromFrames(frames));
    HRESULT hr = writer_->WriteSample(audioStream_, s.Get());
    if (FAILED(hr)) LS_LOG("record: 写音频失败 0x%08lx", (unsigned long)hr);
    return SUCCEEDED(hr);
}

int MediaWriter::VideoBacklog() {
    if (!writer_ || !hasVideo_) return 0;
    MF_SINK_WRITER_STATISTICS st;
    std::memset(&st, 0, sizeof st);
    st.cb = sizeof st;
    if (FAILED(writer_->GetStatistics(videoStream_, &st))) return 0;
    if (st.qwNumSamplesReceived <= st.qwNumSamplesEncoded) return 0;
    return (int)(st.qwNumSamplesReceived - st.qwNumSamplesEncoded);
}

bool MediaWriter::Finish() {
    if (!writer_) return false;
    bool ok = false;
    if (began_) {
        HRESULT hr = writer_->Finalize();
        ok = SUCCEEDED(hr);
        if (!ok) LS_LOG("record: Finalize 失败 0x%08lx", (unsigned long)hr);
    }
    writer_->Release();
    writer_ = nullptr;
    began_ = false;
    return ok;
}

void MediaWriter::Close() {
    if (writer_) writer_->Release();
    writer_ = nullptr;
    began_ = false;
    hasVideo_ = hasAudio_ = false;
}

// ---------------------------------------------------------------- WAV

namespace {
void Put32(BYTE *p, uint32_t v) { std::memcpy(p, &v, 4); }
void Put16(BYTE *p, uint16_t v) { std::memcpy(p, &v, 2); }

void WavHeader(BYTE h[44], uint32_t dataBytes) {
    std::memcpy(h, "RIFF", 4);
    Put32(h + 4, 36 + dataBytes);
    std::memcpy(h + 8, "WAVEfmt ", 8);
    Put32(h + 16, 16);
    Put16(h + 20, 1);
    Put16(h + 22, kAudioChannels);
    Put32(h + 24, kAudioRate);
    Put32(h + 28, kAudioRate * kAudioChannels * 2);
    Put16(h + 32, kAudioChannels * 2);
    Put16(h + 34, 16);
    std::memcpy(h + 36, "data", 4);
    Put32(h + 40, dataBytes);
}
}  // namespace

WavWriter::~WavWriter() { Close(); }

bool WavWriter::Open(const std::wstring &path) {
    Close();
    file_ = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file_ == INVALID_HANDLE_VALUE) return false;
    BYTE h[44];
    WavHeader(h, 0);
    DWORD n = 0;
    bytes_ = 0;
    failed_ = !WriteFile(file_, h, 44, &n, nullptr) || n != 44;
    return !failed_;
}

bool WavWriter::Write(const int16_t *pcm, int frames) {
    if (file_ == INVALID_HANDLE_VALUE || failed_) return false;
    DWORD bytes = (DWORD)frames * kAudioChannels * 2;
    // RIFF 上限 4 GB：超出部分不再写
    if (bytes_ + bytes > 0xFFFFFFFFull - 36) return true;
    DWORD n = 0;
    if (!WriteFile(file_, pcm, bytes, &n, nullptr) || n != bytes) {
        failed_ = true;
        return false;
    }
    bytes_ += bytes;
    return true;
}

bool WavWriter::Finish() {
    if (file_ == INVALID_HANDLE_VALUE) return false;
    BYTE h[44];
    WavHeader(h, (uint32_t)bytes_);
    LARGE_INTEGER zero;
    zero.QuadPart = 0;
    DWORD n = 0;
    bool ok = !failed_ && SetFilePointerEx(file_, zero, nullptr, FILE_BEGIN) && WriteFile(file_, h, 44, &n, nullptr) && n == 44;
    CloseHandle(file_);
    file_ = INVALID_HANDLE_VALUE;
    return ok;
}

void WavWriter::Close() {
    if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
    file_ = INVALID_HANDLE_VALUE;
}

}  // namespace rec
}  // namespace ls
