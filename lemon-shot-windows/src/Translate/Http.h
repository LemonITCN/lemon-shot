// HTTP 请求（spec 5 Windows 部分）：WinHTTP 同步调用，只在后台线程使用。超时 15 秒。
// Win7/8 的 WinHTTP 默认不开 TLS 1.1/1.2，用 WINHTTP_OPTION_SECURE_PROTOCOLS 显式开启；
// 代理：Win8.1+ 用 WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY（跟随系统设置）；更早的系统用 DEFAULT_PROXY，再按 IE 代理设置逐个请求补上。
#pragma once

#include <string>

namespace ls {
namespace http {

struct Response {
    int status = 0;       // 没有拿到响应时为 0
    std::string body;
    std::wstring error;   // 网络错误说明（系统文案），status 为 0 时有值
};

// headers 每行一个 "Name: value"，用 \n 分隔
Response Send(const std::string &method, const std::string &url, const std::string &headers, const std::string &body);

}  // namespace http
}  // namespace ls
