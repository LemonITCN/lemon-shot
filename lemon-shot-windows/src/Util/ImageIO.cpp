#include "Util/ImageIO.h"

#include <objidl.h>
#include <wincodec.h>

#include <cstdio>
#include <cstring>
#include <vector>

#include "UI/Gfx.h"
#include "Util/ComPtr.h"
#include "Util/Log.h"
#include "Util/Util.h"

namespace ls {

bool ImageHasAlpha(const ls_image &img) {
    for (int y = 0; y < img.height; ++y) {
        const uint8_t *p = img.data + (size_t)y * img.stride + 3;
        for (int x = 0; x < img.width; ++x, p += 4)
            if (*p != 255) return true;
    }
    return false;
}

ls_image CloneImage(const ls_image &img) {
    ls_image out = ls_image_alloc(img.width, img.height);
    if (!out.data) return out;
    for (int y = 0; y < img.height; ++y)
        std::memcpy(out.data + (size_t)y * out.stride, img.data + (size_t)y * img.stride, (size_t)img.width * 4);
    return out;
}

static bool StreamBytes(IStream *stream, std::string &out) {
    STATSTG st;
    if (FAILED(stream->Stat(&st, STATFLAG_NONAME))) return false;
    LARGE_INTEGER zero = {};
    stream->Seek(zero, STREAM_SEEK_SET, nullptr);
    out.resize((size_t)st.cbSize.QuadPart);
    ULONG got = 0;
    if (out.empty()) return true;
    if (FAILED(stream->Read(&out[0], (ULONG)out.size(), &got)) || got != out.size()) return false;
    return true;
}

static bool WicEncode(const ls_image &img, const GUID &container, const GUID &pixelFormat, int jpegQuality, std::string &out) {
    IWICImagingFactory *wic = gfx::WIC();
    if (!wic || !img.data) return false;
    ComPtr<IStream> stream;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, stream.Put()))) return false;
    ComPtr<IWICBitmapEncoder> enc;
    if (FAILED(wic->CreateEncoder(container, nullptr, enc.Put()))) return false;
    if (FAILED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache))) return false;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    if (FAILED(enc->CreateNewFrame(frame.Put(), props.Put()))) return false;
    if (jpegQuality > 0 && props) {
        PROPBAG2 opt;
        std::memset(&opt, 0, sizeof opt);
        opt.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_R4;
        v.fltVal = Clamp(jpegQuality, 1, 100) / 100.f;
        props->Write(1, &opt, &v);
    }
    if (FAILED(frame->Initialize(props.Get()))) return false;
    if (FAILED(frame->SetSize((UINT)img.width, (UINT)img.height))) return false;
    WICPixelFormatGUID fmt = pixelFormat;
    if (FAILED(frame->SetPixelFormat(&fmt))) return false;
    ComPtr<IWICBitmap> src;
    if (FAILED(wic->CreateBitmapFromMemory((UINT)img.width, (UINT)img.height, GUID_WICPixelFormat32bppPBGRA,
                                           (UINT)img.stride, (UINT)((size_t)img.stride * img.height), img.data,
                                           src.Put())))
        return false;
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(wic->CreateFormatConverter(conv.Put()))) return false;
    if (FAILED(conv->Initialize(src.Get(), fmt, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
        return false;
    if (FAILED(frame->WriteSource(conv.Get(), nullptr))) return false;
    if (FAILED(frame->Commit()) || FAILED(enc->Commit())) return false;
    return StreamBytes(stream.Get(), out);
}

static uint32_t BE32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

// PDF：把 24 位 PNG 的 IDAT（zlib + PNG 行过滤）原样放进 FlateDecode + Predictor 15，无损且不需要自带压缩库
static bool EncodePdf(const ls_image &img, float scale, std::string &out) {
    std::string png;
    if (!WicEncode(img, GUID_ContainerFormatPng, GUID_WICPixelFormat24bppBGR, 0, png)) return false;
    if (png.size() < 33) return false;
    const uint8_t *p = (const uint8_t *)png.data();
    size_t pos = 8;
    std::string idat;
    int bitDepth = 8, colorType = 2, interlace = 0;
    uint32_t pw = 0, ph = 0;
    while (pos + 12 <= png.size()) {
        uint32_t len = BE32(p + pos);
        const char *type = (const char *)p + pos + 4;
        if (pos + 12 + len > png.size()) return false;
        if (std::memcmp(type, "IHDR", 4) == 0 && len >= 13) {
            pw = BE32(p + pos + 8);
            ph = BE32(p + pos + 12);
            bitDepth = p[pos + 16];
            colorType = p[pos + 17];
            interlace = p[pos + 20];
        } else if (std::memcmp(type, "IDAT", 4) == 0) {
            idat.append((const char *)p + pos + 8, len);
        }
        pos += 12 + len;
    }
    if (idat.empty() || bitDepth != 8 || colorType != 2 || interlace != 0 || pw == 0 || ph == 0) return false;
    if (scale <= 0) scale = 1;
    double pageW = pw / scale, pageH = ph / scale;
    std::vector<size_t> offs;
    out.clear();
    out += "%PDF-1.4\n%\xE2\xE3\xCF\xD3\n";
    auto obj = [&](const std::string &s) {
        offs.push_back(out.size());
        out += s;
    };
    char buf[512];
    obj("1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n");
    obj("2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n");
    snprintf(buf, sizeof buf,
             "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 %.2f %.2f] /Resources << /XObject << /Im0 4 0 R >> >> "
             "/Contents 5 0 R >>\nendobj\n",
             pageW, pageH);
    obj(buf);
    snprintf(buf, sizeof buf,
             "4 0 obj\n<< /Type /XObject /Subtype /Image /Width %u /Height %u /ColorSpace /DeviceRGB /BitsPerComponent 8 "
             "/Filter /FlateDecode /DecodeParms << /Predictor 15 /Colors 3 /BitsPerComponent 8 /Columns %u >> /Length %u "
             ">>\nstream\n",
             pw, ph, pw, (unsigned)idat.size());
    obj(buf);
    out += idat;
    out += "\nendstream\nendobj\n";
    char content[128];
    int cl = snprintf(content, sizeof content, "q %.2f 0 0 %.2f 0 0 cm /Im0 Do Q\n", pageW, pageH);
    snprintf(buf, sizeof buf, "5 0 obj\n<< /Length %d >>\nstream\n%sendstream\nendobj\n", cl, content);
    obj(buf);
    size_t xref = out.size();
    out += "xref\n0 6\n0000000000 65535 f \n";
    for (size_t o : offs) {
        snprintf(buf, sizeof buf, "%010u 00000 n \n", (unsigned)o);
        out += buf;
    }
    snprintf(buf, sizeof buf, "trailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n%u\n%%%%EOF\n", (unsigned)xref);
    out += buf;
    return true;
}

bool EncodeImage(const ls_image &img, const std::string &format, int jpegQuality, float scale, std::string &out) {
    bool alpha = ImageHasAlpha(img);
    if (format == "jpg" || format == "jpeg")
        return WicEncode(img, GUID_ContainerFormatJpeg, GUID_WICPixelFormat24bppBGR, jpegQuality > 0 ? jpegQuality : 90, out);
    if (format == "tiff")
        return WicEncode(img, GUID_ContainerFormatTiff, alpha ? GUID_WICPixelFormat32bppBGRA : GUID_WICPixelFormat24bppBGR, 0,
                         out);
    if (format == "bmp")
        return WicEncode(img, GUID_ContainerFormatBmp, alpha ? GUID_WICPixelFormat32bppBGRA : GUID_WICPixelFormat24bppBGR, 0,
                         out);
    if (format == "pdf") return EncodePdf(img, scale, out);
    return WicEncode(img, GUID_ContainerFormatPng, alpha ? GUID_WICPixelFormat32bppBGRA : GUID_WICPixelFormat24bppBGR, 0, out);
}

bool SaveImageFile(const ls_image &img, const std::wstring &path, const std::string &format, int jpegQuality, float scale) {
    std::string bytes;
    if (!EncodeImage(img, format, jpegQuality, scale, bytes)) {
        LS_LOG("image: 编码失败 %s", format.c_str());
        return false;
    }
    return WriteFileAtomic(path, bytes.data(), bytes.size());
}

static bool DecodeFrom(IWICBitmapDecoder *dec, ls_image &out) {
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(dec->GetFrame(0, frame.Put()))) return false;
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(gfx::WIC()->CreateFormatConverter(conv.Put()))) return false;
    if (FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                WICBitmapPaletteTypeCustom)))
        return false;
    UINT w = 0, h = 0;
    conv->GetSize(&w, &h);
    if (w == 0 || h == 0 || w > 32768 || h > 65536) return false;
    out = ls_image_alloc((int)w, (int)h);
    if (!out.data) return false;
    if (FAILED(conv->CopyPixels(nullptr, (UINT)out.stride, (UINT)((size_t)out.stride * h), out.data))) {
        ls_image_free(&out);
        return false;
    }
    return true;
}

bool DecodeImage(const void *data, size_t size, ls_image &out) {
    out = ls_image{nullptr, 0, 0, 0};
    IWICImagingFactory *wic = gfx::WIC();
    if (!wic || !data || size == 0) return false;
    ComPtr<IWICStream> stream;
    if (FAILED(wic->CreateStream(stream.Put()))) return false;
    if (FAILED(stream->InitializeFromMemory((BYTE *)data, (DWORD)size))) return false;
    ComPtr<IWICBitmapDecoder> dec;
    if (FAILED(wic->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, dec.Put()))) return false;
    return DecodeFrom(dec.Get(), out);
}

bool LoadImageFile(const std::wstring &path, ls_image &out) {
    out = ls_image{nullptr, 0, 0, 0};
    IWICImagingFactory *wic = gfx::WIC();
    if (!wic) return false;
    ComPtr<IWICBitmapDecoder> dec;
    if (FAILED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                              dec.Put())))
        return false;
    return DecodeFrom(dec.Get(), out);
}

static bool OpenClipboardRetry(HWND owner) {
    for (int i = 0; i < 10; ++i) {
        if (OpenClipboard(owner)) return true;
        Sleep(15);
    }
    return false;
}

bool CopyImageToClipboard(HWND owner, const ls_image &img) {
    if (!img.data) return false;
    std::string png;
    EncodeImage(img, "png", 0, 1, png);
    size_t pixBytes = (size_t)img.width * img.height * 4;
    HGLOBAL hdib = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPV5HEADER) + pixBytes);
    if (!hdib) return false;
    uint8_t *p = (uint8_t *)GlobalLock(hdib);
    if (!p) {
        GlobalFree(hdib);
        return false;
    }
    BITMAPV5HEADER *h = (BITMAPV5HEADER *)p;
    std::memset(h, 0, sizeof *h);
    h->bV5Size = sizeof(BITMAPV5HEADER);
    h->bV5Width = img.width;
    h->bV5Height = img.height;  // 自底向上，兼容性最好
    h->bV5Planes = 1;
    h->bV5BitCount = 32;
    h->bV5Compression = BI_BITFIELDS;
    h->bV5SizeImage = (DWORD)pixBytes;
    h->bV5RedMask = 0x00FF0000;
    h->bV5GreenMask = 0x0000FF00;
    h->bV5BlueMask = 0x000000FF;
    h->bV5AlphaMask = 0xFF000000;
    h->bV5CSType = 0x73524742;  // LCS_sRGB
    h->bV5Intent = 4;           // LCS_GM_IMAGES
    uint8_t *dst = p + sizeof(BITMAPV5HEADER);
    for (int y = 0; y < img.height; ++y) {
        const uint8_t *s = img.data + (size_t)(img.height - 1 - y) * img.stride;
        uint8_t *d = dst + (size_t)y * img.width * 4;
        for (int x = 0; x < img.width; ++x, s += 4, d += 4) {
            uint8_t a = s[3];
            if (a == 255 || a == 0) {
                d[0] = s[0];
                d[1] = s[1];
                d[2] = s[2];
            } else {  // 预乘 → 直通
                d[0] = (uint8_t)((s[0] * 255 + a / 2) / a);
                d[1] = (uint8_t)((s[1] * 255 + a / 2) / a);
                d[2] = (uint8_t)((s[2] * 255 + a / 2) / a);
            }
            d[3] = a;
        }
    }
    GlobalUnlock(hdib);
    HGLOBAL hpng = nullptr;
    if (!png.empty()) {
        hpng = GlobalAlloc(GMEM_MOVEABLE, png.size());
        if (hpng) {
            void *q = GlobalLock(hpng);
            if (q) {
                std::memcpy(q, png.data(), png.size());
                GlobalUnlock(hpng);
            } else {
                GlobalFree(hpng);
                hpng = nullptr;
            }
        }
    }
    if (!OpenClipboardRetry(owner)) {
        GlobalFree(hdib);
        if (hpng) GlobalFree(hpng);
        return false;
    }
    EmptyClipboard();
    bool ok = SetClipboardData(CF_DIBV5, hdib) != nullptr;
    if (!ok) GlobalFree(hdib);
    if (hpng) {
        UINT cf = RegisterClipboardFormatW(L"PNG");
        if (!SetClipboardData(cf, hpng)) GlobalFree(hpng);
    }
    CloseClipboard();
    return ok;
}

// CF_DIB / CF_DIBV5 → 补一个 BITMAPFILEHEADER 交给 WIC 的 BMP 解码器
static bool DecodeDib(const uint8_t *dib, size_t size, ls_image &out) {
    if (size < sizeof(BITMAPINFOHEADER)) return false;
    const BITMAPINFOHEADER *bi = (const BITMAPINFOHEADER *)dib;
    size_t headerSize = bi->biSize;
    size_t colors = 0;
    if (bi->biBitCount <= 8) colors = bi->biClrUsed ? bi->biClrUsed : (1u << bi->biBitCount);
    size_t masks = (bi->biCompression == BI_BITFIELDS && headerSize == sizeof(BITMAPINFOHEADER)) ? 12 : 0;
    size_t offBits = sizeof(BITMAPFILEHEADER) + headerSize + masks + colors * 4;
    std::string file(sizeof(BITMAPFILEHEADER), '\0');
    BITMAPFILEHEADER fh;
    std::memset(&fh, 0, sizeof fh);
    fh.bfType = 0x4D42;
    fh.bfSize = (DWORD)(sizeof(BITMAPFILEHEADER) + size);
    fh.bfOffBits = (DWORD)offBits;
    std::memcpy(&file[0], &fh, sizeof fh);
    file.append((const char *)dib, size);
    if (!DecodeImage(file.data(), file.size(), out)) return false;
    // 32 位 BI_RGB 的 DIB 多数程序 alpha 全是 0：视为不透明
    bool allZero = true;
    for (int y = 0; y < out.height && allZero; ++y) {
        const uint8_t *p = out.data + (size_t)y * out.stride;
        for (int x = 0; x < out.width; ++x)
            if (p[x * 4 + 3] != 0) {
                allZero = false;
                break;
            }
    }
    if (allZero && bi->biBitCount == 32) {
        ls_image_free(&out);
        // 用 32bppBGR 重新解释：直接拷贝像素
        int w = bi->biWidth, h = bi->biHeight < 0 ? -bi->biHeight : bi->biHeight;
        size_t pixOff = headerSize + masks;
        if (size < pixOff + (size_t)w * h * 4) return false;
        out = ls_image_alloc(w, h);
        if (!out.data) return false;
        for (int y = 0; y < h; ++y) {
            int sy = bi->biHeight < 0 ? y : h - 1 - y;
            const uint8_t *s = dib + pixOff + (size_t)sy * w * 4;
            uint8_t *d = out.data + (size_t)y * out.stride;
            for (int x = 0; x < w; ++x) {
                d[x * 4 + 0] = s[x * 4 + 0];
                d[x * 4 + 1] = s[x * 4 + 1];
                d[x * 4 + 2] = s[x * 4 + 2];
                d[x * 4 + 3] = 255;
            }
        }
    }
    return true;
}

bool ReadClipboardImage(HWND owner, ls_image &out) {
    out = ls_image{nullptr, 0, 0, 0};
    if (!OpenClipboardRetry(owner)) return false;
    bool ok = false;
    UINT png = RegisterClipboardFormatW(L"PNG");
    UINT fmts[] = {png, CF_DIBV5, CF_DIB};
    for (UINT f : fmts) {
        if (ok || !IsClipboardFormatAvailable(f)) continue;
        HANDLE h = GetClipboardData(f);
        if (!h) continue;
        const void *p = GlobalLock(h);
        size_t size = GlobalSize(h);
        if (p && size) ok = f == png ? DecodeImage(p, size, out) : DecodeDib((const uint8_t *)p, size, out);
        if (p) GlobalUnlock(h);
    }
    CloseClipboard();
    return ok;
}

bool ReadClipboardText(HWND owner, std::wstring &out) {
    out.clear();
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT)) return false;
    if (!OpenClipboardRetry(owner)) return false;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (h) {
        const wchar_t *p = (const wchar_t *)GlobalLock(h);
        if (p) {
            size_t max = GlobalSize(h) / sizeof(wchar_t);
            size_t n = 0;
            while (n < max && p[n]) ++n;
            out.assign(p, n);
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return !out.empty();
}

}  // namespace ls
