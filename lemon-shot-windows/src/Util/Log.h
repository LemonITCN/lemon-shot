// 日志：写 %TEMP%\LemonShot.log（超过 1 MB 启动时清空）。环境变量 LEMONSHOT_LOG=0 关闭。
#pragma once

namespace ls {

void LogInit();
void LogEnable(bool on);
void Log(const char *fmt, ...);
// 自测模式下同时输出到控制台（ssh 会话里能看到）
void LogToConsole(bool on);

}  // namespace ls

#define LS_LOG(...) ::ls::Log(__VA_ARGS__)
