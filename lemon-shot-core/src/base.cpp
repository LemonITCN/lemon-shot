// 基础：版本、图像分配、路径容器。
#include "lemonshot.h"
#include "internal.h"

#include <cstdlib>
#include <cstring>

extern "C" {

const char *ls_version(void) { return LEMONSHOT_VERSION; }

ls_image ls_image_alloc(int32_t width, int32_t height) {
    ls_image img;
    img.data = nullptr;
    img.width = 0;
    img.height = 0;
    img.stride = 0;
    if (width <= 0 || height <= 0) return img;
    size_t stride = (size_t)width * 4;
    void *p = std::calloc((size_t)height, stride);
    if (!p) return img;
    img.data = (uint8_t *)p;
    img.width = width;
    img.height = height;
    img.stride = (int32_t)stride;
    return img;
}

void ls_image_free(ls_image *image) {
    if (!image) return;
    std::free(image->data);
    image->data = nullptr;
    image->width = image->height = image->stride = 0;
}

ls_path *ls_path_create(void) { return new ls_path(); }
void ls_path_destroy(ls_path *path) { delete path; }
int32_t ls_path_count(const ls_path *path) { return path ? (int32_t)path->verbs.size() : 0; }

ls_path_verb ls_path_get(const ls_path *path, int32_t index, ls_point pts[3]) {
    const auto &cmd = path->verbs[(size_t)index];
    for (int i = 0; i < 3; ++i) pts[i] = cmd.pts[i];
    return cmd.verb;
}

}  // extern "C"
