// 长截图拼接。
//
// 每来一帧都和上一张「接上了的帧」比较：
// 1. 从上往下、从下往上找逐行相同（允许极小误差）的行，作为这一对帧的固定页眉/页脚；
// 2. 中间区域每行算 64 段平均亮度的签名，先用每行两个汇总特征粗筛候选位移，再用完整签名精确比较，
//    得到内容向上移动的行数 dy（正数=往下滚）；
// 3. 只追加新露出的行。结果 = 内容（第一帧的页眉在最上面）+ 最后一帧的页脚。
//
// 文档坐标：第一帧的第 y 行就是文档第 y 行，之后每帧的滚动区第 r 行对应文档第 r + s 行（s 为累计位移）。
// 已存内容是文档 [0, content_end) 行；页脚（tail_f 行）单独取自最后一张接上的帧。
// 页脚高度变化时（比如第一对帧才认出页脚，或之前把空白内容误认成页脚）按文档坐标截断/补齐，不会重复或丢行。
#include "internal.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

using lsk::row_ptr;

namespace {

const int kSig = 64;          // 每行签名段数
const int kChunkRows = 256;   // 分块存储每块行数
const int kMinMiddle = 24;    // 中间变化区小于这么多行视为没动
const int kCandidates = 16;   // 粗筛后精确比较的候选数
const int kFeatureCap = 2048; // 粗筛时单行误差上限（避免个别变化行主导）
const int kInfoThreshold = 24;

struct Frame {
    std::vector<uint8_t> px;  // 紧密排列 W*H*4
    std::vector<uint8_t> sig; // H*kSig
    std::vector<int32_t> f1, f2;
    std::vector<uint8_t> info; // 这一行是否有信息量（非纯色或处在横向边缘）

    const uint8_t *row(int y, int W) const { return &px[(size_t)y * W * 4]; }
};

bool rows_equal(const uint8_t *a, const uint8_t *b, int W) {
    if (std::memcmp(a, b, (size_t)W * 4) == 0) return true;
    const int tol = W / 256 > 1 ? W / 256 : 1;
    int bad = 0;
    for (int x = 0; x < W; ++x, a += 4, b += 4) {
        int d = std::abs(a[0] - b[0]);
        d = std::max(d, std::abs(a[1] - b[1]));
        d = std::max(d, std::abs(a[2] - b[2]));
        d = std::max(d, std::abs(a[3] - b[3]));
        if (d > 24 && ++bad > tol) return false;
    }
    return true;
}

void compute_signature(Frame &f, int W, int H) {
    f.sig.resize((size_t)H * kSig);
    f.f1.resize((size_t)H);
    f.f2.resize((size_t)H);
    f.info.resize((size_t)H);
    const int ns = W < kSig ? W : kSig;
    int bounds[kSig + 1];
    for (int k = 0; k <= ns; ++k) bounds[k] = k * W / ns;
    for (int y = 0; y < H; ++y) {
        const uint8_t *p = f.row(y, W);
        uint8_t *s = &f.sig[(size_t)y * kSig];
        int32_t sum = 0, tex = 0;
        for (int k = 0; k < ns; ++k) {
            uint32_t acc = 0;
            const int x0 = bounds[k], x1 = bounds[k + 1];
            for (int x = x0; x < x1; ++x) {
                const uint8_t *q = p + x * 4;
                acc += (uint32_t)(q[2] * 77 + q[1] * 150 + q[0] * 29) >> 8;
            }
            const int n = x1 - x0;
            s[k] = (uint8_t)(n > 0 ? (acc + (uint32_t)n / 2) / (uint32_t)n : 0);
            sum += s[k];
            if (k) tex += std::abs((int)s[k] - (int)s[k - 1]);
        }
        for (int k = ns; k < kSig; ++k) s[k] = 0;
        f.f1[(size_t)y] = sum;
        f.f2[(size_t)y] = tex;
    }
    for (int y = 0; y < H; ++y) {
        bool inf = f.f2[(size_t)y] >= kInfoThreshold;
        if (!inf && y > 0) inf = std::abs(f.f1[(size_t)y] - f.f1[(size_t)y - 1]) >= kInfoThreshold;
        if (!inf && y + 1 < H) inf = std::abs(f.f1[(size_t)y] - f.f1[(size_t)y + 1]) >= kInfoThreshold;
        f.info[(size_t)y] = inf ? 1 : 0;
    }
}

struct Match {
    bool found;
    int dy;
};

}  // namespace

struct ls_stitcher {
    int32_t max_height = 0;
    int W = 0, H = 0;
    bool have_first = false;
    bool maxed = false;
    Frame prev, cur;
    std::vector<std::vector<uint8_t>> chunks;
    int content_end = 0; // 已存内容行数（文档坐标）
    int scroll = 0;      // 最后一张接上的帧的累计位移
    int tail_f = 0;      // 页脚行数（取自 prev）
    bool have_dy = false;
    int last_dy = 0;
    int last_header = 0, last_footer = 0;

    uint8_t *content_row(int y) { return &chunks[(size_t)(y / kChunkRows)][(size_t)(y % kChunkRows) * W * 4]; }
    const uint8_t *content_row(int y) const { return &chunks[(size_t)(y / kChunkRows)][(size_t)(y % kChunkRows) * W * 4]; }
    int total() const { return have_first ? content_end + tail_f : 0; }
    const uint8_t *result_row(int y) const {
        if (y < content_end) return content_row(y);
        return prev.row(H - tail_f + (y - content_end), W);
    }
    void append_rows(const Frame &f, int from, int n) {
        for (int i = 0; i < n; ++i) {
            const int y = content_end + i;
            const size_t ci = (size_t)(y / kChunkRows);
            while (chunks.size() <= ci) chunks.emplace_back((size_t)kChunkRows * W * 4);
            std::memcpy(content_row(y), f.row(from + i, W), (size_t)W * 4);
        }
        content_end += n;
    }
    Match match(int hp, int M) const;
};

// 在中间区 [hp, hp+M) 找位移：cur 第 hp+i 行 对应 prev 第 hp+i+dy 行
Match ls_stitcher::match(int hp, int M) const {
    Match none{false, 0};
    int min_ov = (int)std::ceil(M * 0.15);
    if (min_ov < 16) min_ov = 16;
    const int max_dy = M - min_ov;
    if (max_dy < 1) return none;

    // 粗筛：每行两个汇总特征
    struct Cand {
        float score;
        int dy;
    };
    std::vector<Cand> cands;
    cands.reserve((size_t)(2 * max_dy + 1));
    const int32_t *cf1 = &cur.f1[(size_t)hp], *cf2 = &cur.f2[(size_t)hp];
    const int32_t *pf1 = &prev.f1[(size_t)hp], *pf2 = &prev.f2[(size_t)hp];
    const uint8_t *ci = &cur.info[(size_t)hp], *pi = &prev.info[(size_t)hp];
    for (int dy = -max_dy; dy <= max_dy; ++dy) {
        const int i0 = dy >= 0 ? 0 : -dy;
        const int i1 = dy >= 0 ? M - dy : M;
        int64_t err = 0;
        int cnt = 0;
        for (int i = i0; i < i1; ++i) {
            const int j = i + dy;
            if (!(ci[i] | pi[j])) continue;
            int e = std::abs(cf1[i] - pf1[j]) + std::abs(cf2[i] - pf2[j]);
            err += e < kFeatureCap ? e : kFeatureCap;
            ++cnt;
        }
        if (cnt < 4) continue;
        cands.push_back({(float)err / (float)cnt, dy});
    }
    if (cands.empty()) return none;
    if (cands.size() > (size_t)kCandidates) {
        std::nth_element(cands.begin(), cands.begin() + kCandidates, cands.end(),
                         [](const Cand &a, const Cand &b) { return a.score < b.score; });
        cands.resize((size_t)kCandidates);
    }
    // 上一次的位移（自动滚动时步长恒定）和 0 一定参与精确比较
    auto ensure = [&](int dy) {
        if (dy < -max_dy || dy > max_dy) return;
        for (const Cand &c : cands)
            if (c.dy == dy) return;
        cands.push_back({0, dy});
    };
    ensure(0);
    if (have_dy) ensure(last_dy);

    // 精确比较：64 段签名
    struct Scored {
        float cost;
        float frac;
        float err;
        int dy;
    };
    std::vector<Scored> scored;
    for (const Cand &c : cands) {
        const int dy = c.dy;
        const int i0 = dy >= 0 ? 0 : -dy;
        const int i1 = dy >= 0 ? M - dy : M;
        int n = 0, n_inf = 0, n_match = 0;
        float total_err = 0;
        for (int i = i0; i < i1; ++i) {
            const int j = i + dy;
            const uint8_t *a = &cur.sig[(size_t)(hp + i) * kSig];
            const uint8_t *b = &prev.sig[(size_t)(hp + j) * kSig];
            int e = 0;
            for (int k = 0; k < kSig; ++k) e += std::abs((int)a[k] - (int)b[k]);
            const float ei = (float)e / kSig;
            ++n;
            total_err += ei < 32 ? ei : 32;
            if (ci[i] | pi[j]) {
                ++n_inf;
                if (ei <= 3.0f) ++n_match;
            }
        }
        if (n_inf < 4 || n == 0) continue;
        const float frac = (float)n_match / (float)n_inf;
        const float err = total_err / (float)n;
        scored.push_back({(1 - frac) * 100 + err, frac, err, dy});
    }
    if (scored.empty()) return none;
    float best = scored[0].cost;
    for (const Scored &s : scored) best = std::min(best, s.cost);
    // 代价相近（重复图案）时取离上次位移最近的；没有上次时取位移绝对值最小的
    const Scored *pick = nullptr;
    for (const Scored &s : scored) {
        if (s.cost > best + 0.5f) continue;
        if (!pick) {
            pick = &s;
            continue;
        }
        const int ref = have_dy ? last_dy : 0;
        if (std::abs(s.dy - ref) < std::abs(pick->dy - ref) || (std::abs(s.dy - ref) == std::abs(pick->dy - ref) && s.cost < pick->cost))
            pick = &s;
    }
    if (!pick || pick->frac < 0.85f || pick->err > 6.0f) return none;
    return Match{true, pick->dy};
}

extern "C" {

ls_stitcher *ls_stitch_create(int32_t max_height) {
    ls_stitcher *st = new ls_stitcher();
    st->max_height = max_height > 0 ? max_height : 0x7FFFFFFF;
    return st;
}

void ls_stitch_destroy(ls_stitcher *st) { delete st; }

int32_t ls_stitch_push(ls_stitcher *st, const ls_image *frame, ls_stitch_result *out) {
    ls_stitch_result res;
    std::memset(&res, 0, sizeof res);
    res.status = LS_STITCH_NO_MATCH;
    auto finish = [&](int32_t status, int32_t added, int32_t rc) {
        res.status = status;
        res.added = added;
        res.total_height = st ? st->total() : 0;
        res.header = st ? st->last_header : 0;
        res.footer = st ? st->last_footer : 0;
        if (out) *out = res;
        return rc;
    };
    if (!st || !frame || !frame->data || frame->width <= 0 || frame->height <= 0) return finish(LS_STITCH_NO_MATCH, 0, -1);
    if (!st->have_first) {
        const int W = frame->width, H = frame->height;
        st->W = W;
        st->H = H;
        st->prev.px.resize((size_t)W * H * 4);
        for (int y = 0; y < H; ++y) std::memcpy(&st->prev.px[(size_t)y * W * 4], row_ptr(frame, y), (size_t)W * 4);
        compute_signature(st->prev, W, H);
        int n = H;
        if (n >= st->max_height) {
            n = st->max_height;
            st->maxed = true;
        }
        st->append_rows(st->prev, 0, n);
        st->tail_f = 0;
        st->scroll = 0;
        st->have_first = true;
        return finish(LS_STITCH_FIRST, n, 0);
    }
    const int W = st->W, H = st->H;
    if (frame->width != W || frame->height != H) return finish(LS_STITCH_NO_MATCH, 0, -1);
    if (st->maxed) return finish(LS_STITCH_MAX, 0, 0);

    Frame &cur = st->cur;
    cur.px.resize((size_t)W * H * 4);
    for (int y = 0; y < H; ++y) std::memcpy(&cur.px[(size_t)y * W * 4], row_ptr(frame, y), (size_t)W * 4);

    // 固定页眉/页脚：逐行比较原始像素
    int hp = 0;
    while (hp < H && rows_equal(cur.row(hp, W), st->prev.row(hp, W), W)) ++hp;
    if (hp == H) return finish(LS_STITCH_UNCHANGED, 0, 0);
    int fp = 0;
    while (fp < H - hp && rows_equal(cur.row(H - 1 - fp, W), st->prev.row(H - 1 - fp, W), W)) ++fp;
    const int M = H - hp - fp;
    if (M < kMinMiddle) return finish(LS_STITCH_UNCHANGED, 0, 0);

    compute_signature(cur, W, H);
    const Match m = st->match(hp, M);
    if (!m.found) return finish(LS_STITCH_NO_MATCH, 0, 0);
    if (m.dy == 0) return finish(LS_STITCH_UNCHANGED, 0, 0);
    if (m.dy < 0) return finish(LS_STITCH_BACKWARD, 0, 0);

    const int old_total = st->total();
    const int dy = m.dy;
    const int s_prev = st->scroll;
    const int s = s_prev + dy;
    // 页脚变高：上一帧底部那几行其实是页脚，从内容里去掉（它们会在本帧对应的文档位置重新追加）
    st->content_end = std::min(st->content_end, s_prev + H - fp);
    int from = st->content_end - s; // 本帧中接着已存内容的那一行
    if (from < hp) {
        // 极端情况（页脚骤减超过重叠区）：中间缺的行已经看不到了，只能跳过
        st->content_end += hp - from;
        from = hp;
    }
    int n = (s + H - fp) - st->content_end;
    if (n < 0) n = 0;
    bool hit_max = false;
    if (st->content_end + n + fp >= st->max_height) {
        n = std::max(0, st->max_height - fp - st->content_end);
        hit_max = true;
    }
    // content_end 可能因上面的跳过而越过已分配的块，先补齐
    while ((int)st->chunks.size() * kChunkRows < st->content_end)
        st->chunks.emplace_back((size_t)kChunkRows * W * 4);
    st->append_rows(cur, from, n);
    st->scroll = s;
    st->tail_f = fp;
    st->last_header = hp;
    st->last_footer = fp;
    st->have_dy = true;
    st->last_dy = dy;
    std::swap(st->prev, st->cur);
    if (hit_max) st->maxed = true;
    return finish(hit_max ? LS_STITCH_MAX : LS_STITCH_APPENDED, st->total() - old_total, 0);
}

int32_t ls_stitch_height(const ls_stitcher *st) { return st ? st->total() : 0; }

int32_t ls_stitch_render(const ls_stitcher *st, ls_image *out) {
    if (!out) return -1;
    out->data = nullptr;
    out->width = out->height = out->stride = 0;
    if (!st || !st->have_first) return -1;
    const int T = st->total();
    *out = ls_image_alloc(st->W, T);
    if (!out->data) return -1;
    for (int y = 0; y < T; ++y) std::memcpy(row_ptr(out, y), st->result_row(y), (size_t)st->W * 4);
    return 0;
}

static const uint8_t *stitch_row(const void *ctx, int32_t y) { return ((const ls_stitcher *)ctx)->result_row(y); }

int32_t ls_stitch_preview(const ls_stitcher *st, int32_t max_w, int32_t max_h, ls_image *out) {
    if (!out) return -1;
    out->data = nullptr;
    out->width = out->height = out->stride = 0;
    if (!st || !st->have_first || max_w <= 0 || max_h <= 0) return -1;
    const int T = st->total();
    double scale = std::min((double)max_w / st->W, (double)max_h / T);
    if (scale > 1) scale = 1;
    int dw = (int)std::lround(st->W * scale), dh = (int)std::lround(T * scale);
    dw = lsk::clampi(dw, 1, max_w);
    dh = lsk::clampi(dh, 1, max_h);
    return lsk::resize_rows(stitch_row, st, st->W, T, dw, dh, out);
}

}  // extern "C"
