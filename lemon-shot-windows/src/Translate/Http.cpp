#include "Translate/Http.h"

#include <windows.h>
#include <winhttp.h>

#include <vector>

#include "Util/Api.h"
#include "Util/Log.h"
#include "Util/Util.h"

namespace ls {
namespace http {

namespace {

const DWORD kTimeoutMs = 15000;
const size_t kMaxBody = 8u << 20;

INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
HINTERNET g_session = nullptr;   // WinHTTP 会话句柄可以跨线程共用，进程内常驻
bool g_ieProxy = false;           // Win7/8：按 IE 代理设置逐个请求设代理
std::wstring g_ieProxyServer, g_ieProxyBypass, g_ieAutoConfigUrl;
bool g_ieAutoDetect = false;
volatile LONG g_autoDetectFailed = 0;  // 自动检测（WPAD）失败过就不再每次都等它

BOOL CALLBACK InitSession(PINIT_ONCE, PVOID, PVOID *) {
    const wchar_t *agent = L"LemonShot/0.1";
    bool modern = api::IsWin81OrLater();
    if (modern) g_session = WinHttpOpen(agent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!g_session) g_session = WinHttpOpen(agent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!g_session) {
        LS_LOG("http: WinHttpOpen 失败 %lu", GetLastError());
        return TRUE;
    }
    WinHttpSetTimeouts(g_session, (int)kTimeoutMs, (int)kTimeoutMs, (int)kTimeoutMs, (int)kTimeoutMs);
    if (!modern) {
        // Win7/8：默认只有 SSL3/TLS1.0，显式打开 TLS 1.1/1.2（Win7 需要 KB3140245，没有时退回只设 TLS 1.0）
        DWORD protos = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_1 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
        if (!WinHttpSetOption(g_session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protos, sizeof protos)) {
            LS_LOG("http: 打开 TLS 1.2 失败 %lu（系统可能缺 KB3140245）", GetLastError());
            protos = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1;
            WinHttpSetOption(g_session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protos, sizeof protos);
        }
        // DEFAULT_PROXY 只读 netsh winhttp 的设置：再看一下 IE（系统）代理设置
        WINHTTP_CURRENT_USER_IE_PROXY_CONFIG ie;
        ZeroMemory(&ie, sizeof ie);
        if (WinHttpGetIEProxyConfigForCurrentUser(&ie)) {
            if (ie.lpszProxy) g_ieProxyServer = ie.lpszProxy;
            if (ie.lpszProxyBypass) g_ieProxyBypass = ie.lpszProxyBypass;
            if (ie.lpszAutoConfigUrl) g_ieAutoConfigUrl = ie.lpszAutoConfigUrl;
            g_ieAutoDetect = ie.fAutoDetect != FALSE;
            g_ieProxy = !g_ieProxyServer.empty() || !g_ieAutoConfigUrl.empty() || g_ieAutoDetect;
            if (ie.lpszProxy) GlobalFree(ie.lpszProxy);
            if (ie.lpszProxyBypass) GlobalFree(ie.lpszProxyBypass);
            if (ie.lpszAutoConfigUrl) GlobalFree(ie.lpszAutoConfigUrl);
        }
    }
    return TRUE;
}

HINTERNET Session() {
    InitOnceExecuteOnce(&g_once, &InitSession, nullptr, nullptr);
    return g_session;
}

std::wstring ErrorMessage(DWORD err) {
    wchar_t *buf = nullptr;
    DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_FROM_SYSTEM |
                                 FORMAT_MESSAGE_IGNORE_INSERTS,
                             GetModuleHandleW(L"winhttp.dll"), err, 0, (LPWSTR)&buf, 0, nullptr);
    std::wstring s;
    if (n && buf) s.assign(buf, n);
    if (buf) LocalFree(buf);
    s = Trim(s);
    if (s.empty()) s = FormatW(L"WinHTTP %lu", err);
    return s;
}

// Win7/8：按 IE 代理设置给这个请求设代理（PAC/自动检测用 WinHttpGetProxyForUrl）
void ApplyIeProxy(HINTERNET req, const std::wstring &url) {
    if (!g_ieProxy) return;
    WINHTTP_PROXY_INFO info;
    ZeroMemory(&info, sizeof info);
    bool got = false;
    bool autoDetect = g_ieAutoDetect && g_autoDetectFailed == 0;
    if (autoDetect || !g_ieAutoConfigUrl.empty()) {
        WINHTTP_AUTOPROXY_OPTIONS ap;
        ZeroMemory(&ap, sizeof ap);
        if (!g_ieAutoConfigUrl.empty()) {
            ap.dwFlags = WINHTTP_AUTOPROXY_CONFIG_URL;
            ap.lpszAutoConfigUrl = g_ieAutoConfigUrl.c_str();
        } else {
            ap.dwFlags = WINHTTP_AUTOPROXY_AUTO_DETECT;
            ap.dwAutoDetectFlags = WINHTTP_AUTO_DETECT_TYPE_DHCP | WINHTTP_AUTO_DETECT_TYPE_DNS_A;
        }
        ap.fAutoLogonIfChallenged = TRUE;
        got = WinHttpGetProxyForUrl(Session(), url.c_str(), &ap, &info) != FALSE;
        if (!got && g_ieAutoConfigUrl.empty()) InterlockedExchange(&g_autoDetectFailed, 1);
        if (got) {
            WinHttpSetOption(req, WINHTTP_OPTION_PROXY, &info, sizeof info);
            if (info.lpszProxy) GlobalFree(info.lpszProxy);
            if (info.lpszProxyBypass) GlobalFree(info.lpszProxyBypass);
            return;
        }
    }
    if (!g_ieProxyServer.empty()) {
        info.dwAccessType = WINHTTP_ACCESS_TYPE_NAMED_PROXY;
        info.lpszProxy = (LPWSTR)g_ieProxyServer.c_str();
        info.lpszProxyBypass = g_ieProxyBypass.empty() ? nullptr : (LPWSTR)g_ieProxyBypass.c_str();
        WinHttpSetOption(req, WINHTTP_OPTION_PROXY, &info, sizeof info);
    }
}

}  // namespace

Response Send(const std::string &method, const std::string &url, const std::string &headers, const std::string &body) {
    Response out;
    HINTERNET session = Session();
    if (!session) {
        out.error = ErrorMessage(GetLastError());
        return out;
    }
    std::wstring wurl = Widen(url);
    URL_COMPONENTS uc;
    ZeroMemory(&uc, sizeof uc);
    uc.dwStructSize = sizeof uc;
    uc.dwSchemeLength = (DWORD)-1;
    uc.dwHostNameLength = (DWORD)-1;
    uc.dwUrlPathLength = (DWORD)-1;
    uc.dwExtraInfoLength = (DWORD)-1;
    if (!WinHttpCrackUrl(wurl.c_str(), (DWORD)wurl.size(), 0, &uc) || !uc.lpszHostName) {
        out.error = ErrorMessage(GetLastError());
        return out;
    }
    std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
    std::wstring path = uc.lpszUrlPath ? std::wstring(uc.lpszUrlPath, uc.dwUrlPathLength) : std::wstring();
    if (uc.lpszExtraInfo) path += std::wstring(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    if (path.empty()) path = L"/";
    bool https = uc.nScheme == INTERNET_SCHEME_HTTPS;

    HINTERNET conn = WinHttpConnect(session, host.c_str(), uc.nPort, 0);
    if (!conn) {
        out.error = ErrorMessage(GetLastError());
        return out;
    }
    std::wstring verb = Widen(method.empty() ? std::string("GET") : method);
    HINTERNET req = WinHttpOpenRequest(conn, verb.c_str(), path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                       https ? WINHTTP_FLAG_SECURE : 0);
    if (!req) {
        out.error = ErrorMessage(GetLastError());
        WinHttpCloseHandle(conn);
        return out;
    }
    if (!api::IsWin81OrLater()) ApplyIeProxy(req, wurl);
    // 逐行加请求头（Host 由 WinHTTP 按地址自动填，和签名里的一致）
    for (const std::string &line : Split(headers, '\n')) {
        std::string l = line;
        while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back();
        size_t colon = l.find(':');
        if (l.empty() || colon == std::string::npos || colon == 0) continue;
        if (ToLower(l.substr(0, colon)) == "host") continue;
        std::wstring wl = Widen(l);
        WinHttpAddRequestHeaders(req, wl.c_str(), (DWORD)wl.size(), WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
    }
    double t0 = NowSeconds();
    BOOL ok = WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.data(),
                                 (DWORD)body.size(), (DWORD)body.size(), 0);
    if (ok) ok = WinHttpReceiveResponse(req, nullptr);
    if (ok) {
        DWORD code = 0, len = sizeof code;
        if (WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &len,
                                WINHTTP_NO_HEADER_INDEX))
            out.status = (int)code;
        std::vector<char> buf;
        for (;;) {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(req, &avail)) {
                ok = FALSE;
                break;
            }
            if (avail == 0) break;
            if (out.body.size() + avail > kMaxBody) break;
            buf.resize(avail);
            DWORD read = 0;
            if (!WinHttpReadData(req, buf.data(), avail, &read)) {
                ok = FALSE;
                break;
            }
            if (read == 0) break;
            out.body.append(buf.data(), read);
        }
    }
    if (!ok) {
        DWORD err = GetLastError();
        out.error = ErrorMessage(err);
        if (out.status == 0) out.body.clear();
        LS_LOG("http: %ls 失败 %lu（%.0f ms）", host.c_str(), err, (NowSeconds() - t0) * 1000.0);
    }
    WinHttpCloseHandle(req);
    WinHttpCloseHandle(conn);
    return out;
}

}  // namespace http
}  // namespace ls
