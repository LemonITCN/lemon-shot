#include "Record/Record.h"

namespace ls {
namespace record {

void Start() {}
void StartWithRegion(HMONITOR, RECT) {}
void TogglePause() {}
void TogglePen() {}
void Stop() {}
bool IsRecording() { return false; }
bool IsPaused() { return false; }

}  // namespace record
}  // namespace ls
