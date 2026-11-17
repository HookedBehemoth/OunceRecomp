// The BC encoders and decoders the texture converter calls (src/native.rs): bc7enc_rdo's bc7e (BC7, ISPC) and rgbcx
// (BC1, BC4, BC5), with their decoders for the quality check.
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

#include <immintrin.h>

#include "bc7decomp.h"
#include "bc7e_ispc.h"
#include "rgbcx.h"

#define HK_API extern "C"
static void gather(const uint8_t* rgba, uint32_t w, uint32_t h, uint32_t bx, uint32_t by, uint8_t* px) {
    for (uint32_t y = 0; y < 4; y++)
        for (uint32_t x = 0; x < 4; x++) {
            const uint32_t sx = std::min(bx * 4 + x, w - 1), sy = std::min(by * 4 + y, h - 1);
            std::memcpy(px + (y * 4 + x) * 4, rgba + ((uint64_t)sy * w + sx) * 4, 4);
        }
}
static void init_once() {
    static std::once_flag once;
    std::call_once(once, [] {
        rgbcx::init(rgbcx::bc1_approx_mode::cBC1Ideal);
        ispc::bc7e_compress_block_init();
    });
}

/* BC7 keeps the texels of alpha 0 at 0: a block that holds any and decodes one of them above 0 is encoded again with the
   alpha's weight raised (x16, then x256), and the new block is kept when it leaves fewer such texels. With equal weights
   the encoder trades an alpha of 1..100 there for colour error, and a texture drawn with its alpha as coverage shows
   those texels as thin lines over its clear area. The colour given is not changed */
static int clear_leaks(const uint8_t* px, const uint8_t* blk) {
    bc7decomp::color_rgba d[16];
    bc7decomp::unpack_bc7(blk, d);
    int n = 0;
    for (int t = 0; t < 16; t++) n += px[t * 4 + 3] == 0 && d[t].a != 0;
    return n;
}
static void bc7_keep_clear(const uint8_t* rgba, uint32_t w, uint32_t h, uint8_t* out, const ispc::bc7e_compress_block_params& base) {
    const uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
    std::vector<uint64_t> redo;
    uint8_t px[64];
    for (uint64_t i = 0, n = (uint64_t)bw * bh; i < n; i++) {
        gather(rgba, w, h, (uint32_t)(i % bw), (uint32_t)(i / bw), px);
        if (clear_leaks(px, out + i * 16)) redo.push_back(i);
    }
    for (const uint32_t boost : {16u, 256u}) {
        if (redo.empty()) return;
        ispc::bc7e_compress_block_params p = base;
        p.m_weights[3] = std::max(1u, base.m_weights[3]) * boost;
        std::vector<uint64_t> left;
        uint8_t blk[16];
        for (const uint64_t i : redo) {
            gather(rgba, w, h, (uint32_t)(i % bw), (uint32_t)(i / bw), px);
            ispc::bc7e_compress_blocks(1, reinterpret_cast<uint64_t*>(blk), reinterpret_cast<const uint32_t*>(px), &p);
            const int now = clear_leaks(px, blk), was = clear_leaks(px, out + i * 16);
            if (now < was) std::memcpy(out + i * 16, blk, 16);
            if (std::min(now, was)) left.push_back(i);
        }
        redo.swap(left);
    }
}
/* rgbcx::encode_bc4_hq's search, the same blocks out: every endpoint pair within the radius in its order, each texel's
   selector the first nearest value, the first pair of least total error kept. The 16 texels are one AVX2 vector
   (errors as u16), without rgbcx's early exits. */
static inline int clampi(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }
static void bc4_values(uint8_t* v, uint32_t l, uint32_t h) {
    if (l > h) {
        v[0] = (uint8_t)l, v[1] = (uint8_t)h;
        for (uint32_t k = 1; k <= 6; k++) v[k + 1] = (uint8_t)((l * (7 - k) + h * k) / 7);
    } else {
        v[0] = (uint8_t)l, v[1] = (uint8_t)h;
        for (uint32_t k = 1; k <= 4; k++) v[k + 1] = (uint8_t)((l * (5 - k) + h * k) / 5);
        v[6] = 0, v[7] = 255;
    }
}
static void bc4_hq(uint8_t* dst, const uint8_t* px, uint32_t stride, int rad) {
    int32_t p[16];
    uint32_t lo = 255, hi = 0;
    for (int i = 0; i < 16; i++) {
        p[i] = px[i * stride];
        lo = std::min<uint32_t>(lo, p[i]), hi = std::max<uint32_t>(hi, p[i]);
    }
    if (lo == hi) {   // (BC4_USE_ALL_MODES includes mode 6: both endpoints the value, selectors 0)
        dst[0] = dst[1] = (uint8_t)lo;
        std::memset(dst + 2, 0, 6);
        return;
    }
    alignas(32) int16_t pa[16];
    for (int i = 0; i < 16; i++) pa[i] = (int16_t)p[i];
    const __m256i pv = _mm256_load_si256(reinterpret_cast<const __m256i*>(pa));
    uint32_t best_err = UINT32_MAX;
    for (int mode = 0; mode < 2; mode++)
        for (int dl = -rad; dl <= rad; dl++)
            for (int dh = -rad; dh <= rad; dh++) {
                uint8_t e0 = (uint8_t)clampi((int)hi + dh), e1 = (uint8_t)clampi((int)lo + dl);
                if (e0 == e1) continue;
                if ((mode == 0) == (e0 <= e1)) std::swap(e0, e1);
                uint8_t v[8];
                bc4_values(v, e0, e1);
                __m256i err = _mm256_set1_epi16(-1), sel = _mm256_setzero_si256();
                for (int j = 0; j < 8; j++) {
                    const __m256i d = _mm256_sub_epi16(_mm256_set1_epi16(v[j]), pv);
                    const __m256i e = _mm256_mullo_epi16(d, d);   // (u16: at most 255^2)
                    const __m256i lt = _mm256_xor_si256(_mm256_cmpeq_epi16(_mm256_min_epu16(e, err), err), _mm256_set1_epi16(-1));
                    err = _mm256_blendv_epi8(err, e, lt);
                    sel = _mm256_blendv_epi8(sel, _mm256_set1_epi16((short)j), lt);
                }
                // (the 16 errors summed in 32 bits)
                const __m256i e32 = _mm256_madd_epi16(_mm256_unpacklo_epi16(err, _mm256_setzero_si256()), _mm256_set1_epi16(1));
                const __m256i f32 = _mm256_madd_epi16(_mm256_unpackhi_epi16(err, _mm256_setzero_si256()), _mm256_set1_epi16(1));
                __m256i t = _mm256_add_epi32(e32, f32);
                __m128i q = _mm_add_epi32(_mm256_castsi256_si128(t), _mm256_extracti128_si256(t, 1));
                q = _mm_add_epi32(q, _mm_shuffle_epi32(q, 0x4e));
                q = _mm_add_epi32(q, _mm_shuffle_epi32(q, 0xb1));
                const uint32_t total = (uint32_t)_mm_cvtsi128_si32(q);
                if (total < best_err) {
                    best_err = total;
                    alignas(32) uint16_t sl[16];
                    _mm256_store_si256(reinterpret_cast<__m256i*>(sl), sel);
                    uint64_t bits = 0;
                    for (int i = 0; i < 16; i++) bits |= (uint64_t)sl[i] << (i * 3);
                    dst[0] = e0, dst[1] = e1;
                    for (int k = 0; k < 6; k++) dst[2 + k] = (uint8_t)(bits >> (8 * k));
                }
            }
}

// kind: 1 BC1 (opaque, no 3-colour black: index 3 would be transparent in BC1_RGBA), 4 BC4 (channel c0), 5 BC5 (c0 ->
// R, c1 -> G), 7 BC7 (bc7e; level 0..6 = ultrafast..slowest; RGBA as given). rgba: w x h x 4 bytes, packed rows. out:
// ceil(w/4) * ceil(h/4) blocks, row-major. Edge blocks repeat the last row and column. Returns 0, or -2 (no such kind).
HK_API int hk_encode(int kind, uint32_t w, uint32_t h, const uint8_t* rgba, uint8_t* out, int c0, int c1, int level,
                     int radius) {
    init_once();
    const uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
    if (kind == 7) {
        ispc::bc7e_compress_block_params p;
        switch (level) {
            case 0: ispc::bc7e_compress_block_params_init_ultrafast(&p, false); break;
            case 1: ispc::bc7e_compress_block_params_init_veryfast(&p, false); break;
            case 2: ispc::bc7e_compress_block_params_init_fast(&p, false); break;
            case 3: ispc::bc7e_compress_block_params_init_basic(&p, false); break;
            case 4: ispc::bc7e_compress_block_params_init_slow(&p, false); break;
            case 5: ispc::bc7e_compress_block_params_init_veryslow(&p, false); break;
            default: ispc::bc7e_compress_block_params_init_slowest(&p, false); break;
        }
        std::vector<uint8_t> px(64 * 64);   // 64 blocks a call
        for (uint64_t i = 0, n = (uint64_t)bw * bh; i < n;) {
            const uint32_t k = (uint32_t)std::min<uint64_t>(64, n - i);
            for (uint32_t j = 0; j < k; j++) gather(rgba, w, h, (uint32_t)((i + j) % bw), (uint32_t)((i + j) / bw), &px[j * 64]);
            ispc::bc7e_compress_blocks(k, reinterpret_cast<uint64_t*>(out + i * 16), reinterpret_cast<const uint32_t*>(px.data()), &p);
            i += k;
        }
        bc7_keep_clear(rgba, w, h, out, p);
        return 0;
    }
    uint8_t px[64];
    const uint32_t bpb = kind == 5 ? 16 : 8;
    for (uint32_t by = 0; by < bh; by++)
        for (uint32_t bx = 0; bx < bw; bx++) {
            gather(rgba, w, h, bx, by, px);
            uint8_t* o = out + ((uint64_t)by * bw + bx) * bpb;
            if (kind == 1) rgbcx::encode_bc1(level < 0 ? rgbcx::MAX_LEVEL : (uint32_t)level, o, px, true, false);
            else if (kind == 4) bc4_hq(o, px + c0, 4, radius);
            else if (kind == 5) bc4_hq(o, px + c0, 4, radius), bc4_hq(o + 8, px + c1, 4, radius);
            else return -2;
        }
    return 0;
}

/// Blocks -> RGBA (w x h x 4, packed rows, cropped). BC4: R, G = B = 0, A = 255; BC5: R, G, B = 0, A = 255.
HK_API int hk_decode(int kind, uint32_t w, uint32_t h, const uint8_t* blocks, uint8_t* rgba) {
    init_once();
    const uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4, bpb = (kind == 1 || kind == 4) ? 8 : 16;
    uint8_t px[64];
    for (uint32_t by = 0; by < bh; by++)
        for (uint32_t bx = 0; bx < bw; bx++) {
            const uint8_t* b = blocks + ((uint64_t)by * bw + bx) * bpb;
            for (int i = 0; i < 16; i++) { px[i * 4] = px[i * 4 + 1] = px[i * 4 + 2] = 0; px[i * 4 + 3] = 255; }
            if (kind == 1) rgbcx::unpack_bc1(b, px, true, rgbcx::bc1_approx_mode::cBC1Ideal);
            else if (kind == 4) rgbcx::unpack_bc4(b, px, 4);
            else if (kind == 5) rgbcx::unpack_bc5(b, px, 0, 1, 4);
            else if (kind == 7) bc7decomp::unpack_bc7(b, reinterpret_cast<bc7decomp::color_rgba*>(px));
            else return -2;
            for (uint32_t y = 0; y < 4 && by * 4 + y < h; y++)
                for (uint32_t x = 0; x < 4 && bx * 4 + x < w; x++)
                    std::memcpy(rgba + ((uint64_t)(by * 4 + y) * w + bx * 4 + x) * 4, px + (y * 4 + x) * 4, 4);
        }
    return 0;
}

/// rgbcx's own BC4 search (the check that bc4_hq gives its blocks)
HK_API void hk_bc4_rgbcx(uint8_t* out, const uint8_t* px16, int radius) {
    init_once();
    rgbcx::encode_bc4_hq(out, px16, 1, (uint32_t)radius, rgbcx::BC4_USE_ALL_MODES);
}
HK_API void hk_bc4_fast(uint8_t* out, const uint8_t* px16, int radius) { bc4_hq(out, px16, 1, radius); }
