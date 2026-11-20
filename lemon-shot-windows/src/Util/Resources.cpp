#include "Util/Resources.h"

#include <windows.h>

namespace ls {

std::string LoadResourceData(int id) {
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    if (!r) return std::string();
    HGLOBAL g = LoadResource(nullptr, r);
    if (!g) return std::string();
    const void *p = LockResource(g);
    DWORD n = SizeofResource(nullptr, r);
    if (!p || n == 0) return std::string();
    return std::string((const char *)p, n);
}

}  // namespace ls
