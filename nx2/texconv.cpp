// texconv: the native half of nx2/prepare_textures.py (the build-time texture converter, docs: that script's docstring).
// A shared library the script loads with ctypes and builds itself on first use:
//   - Tegra block-linear <-> linear (one mip of one layer), the GOB rule of host/nvn_layout.c;
//   - an ASTC block scan (HDR endpoint modes, HDR void-extent, reserved block modes): NVN only has LDR ASTC formats;
//   - with HK_BC7ENC (bc7enc_rdo by Richard Geldreich, MIT / public domain; bc7e.ispc, Apache-2.0, Binomial LLC):
//     BC7 (bc7e), BC5 / BC4 / BC1 (rgbcx) encoders and the matching decoders, for the converter's quality check.
// Without HK_BC7ENC (CI, a PC without the encoder sources) only the layout and the scan are built: hk_has_encoders() = 0.
// Build-time tool only: nothing of this is linked into the game.
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#ifdef HK_BC7ENC
#include "bc7decomp.h"
#include "bc7e_ispc.h"
#include "rgbcx.h"
#endif

#if defined(_WIN32)
#define HK_API extern "C" __declspec(dllexport)
#else
#define HK_API extern "C" __attribute__((visibility("default")))
#endif

HK_API int hk_version() { return 1; }

HK_API int hk_has_encoders() {
#ifdef HK_BC7ENC
    return 1;
#else
    return 0;
#endif
}

// ---- block-linear ----------------------------------------------------------------------------------------------------
// GOB = 64 bytes x 8 rows (512 B); a block is 1 GOB wide and 1 << bh_log2 GOBs tall, blocks row-major. Within a GOB the
// byte (x, y) is at (x%64/32)*256 + (y%8/4)*128 + (x%32/16)*64 + (y%4/2)*32 + (y%2)*16 + x%16: 16 bytes in a row are
// contiguous. This is the Switch 2's GOB (gpu/vk/vk_texture.cpp's, suyu's with the Switch 2 swizzle; M4d): the Switch 1's
// (x%64/32)*256 + (y%8/2)*64 + (x%32/16)*32 + (y%2)*16 + x%16 shuffled the 16-byte pieces of every GOB, which a texture's
// last, partial GOB column showed (ASTC blocks of zeros: magenta in the decoder)
static inline uint64_t bl_offset(uint32_t x, uint32_t y, uint32_t gobs_x, uint32_t bh_log2) {
    const uint32_t rows_per_block = 8u << bh_log2;
    const uint64_t block = (uint64_t)(y / rows_per_block) * gobs_x + x / 64;
    return block * (512ull << bh_log2) + (uint64_t)((y % rows_per_block) / 8) * 512 + ((x % 64) / 32) * 256 +
           ((y % 8) / 4) * 128 + ((x % 32) / 16) * 64 + ((y % 4) / 2) * 32 + (y % 2) * 16 + (x % 16);
}

/// Bytes of one block-linear surface of width_bytes x rows at this block height (the mip's own, already reduced)
HK_API uint64_t hk_block_linear_size(uint32_t width_bytes, uint32_t rows, uint32_t bh_log2) {
    const uint64_t gobs_x = (width_bytes + 63) / 64, blocks_y = (rows + (8u << bh_log2) - 1) / (8u << bh_log2);
    return gobs_x * blocks_y * (512ull << bh_log2);
}

/// to_linear = 1: block-linear `bl` (bl_len bytes; reads past it give zeros) -> `lin`, rows of width_bytes, packed.
/// to_linear = 0: `lin` -> `bl` (bl_len bytes, hk_block_linear_size; the padding is left as it is).
HK_API void hk_block_linear(uint8_t* bl, uint64_t bl_len, uint8_t* lin, uint32_t width_bytes, uint32_t rows,
                            uint32_t bh_log2, int to_linear) {
    const uint32_t gobs_x = (width_bytes + 63) / 64;
    for (uint32_t y = 0; y < rows; y++) {
        uint8_t* row = lin + (uint64_t)y * width_bytes;
        for (uint32_t x = 0; x < width_bytes; x += 16) {
            const uint32_t n = std::min(16u, width_bytes - x);
            const uint64_t o = bl_offset(x, y, gobs_x, bh_log2);
            if (to_linear) {
                if (o + n <= bl_len) std::memcpy(row + x, bl + o, n);
                else {
                    std::memset(row + x, 0, n);
                    if (o < bl_len) std::memcpy(row + x, bl + o, (size_t)(bl_len - o));
                }
            } else if (o + n <= bl_len) {
                std::memcpy(bl + o, row + x, n);
            }
        }
    }
}

// ---- ASTC block scan -------------------------------------------------------------------------------------------------
static inline uint32_t bits(const uint8_t* b, uint32_t pos, uint32_t n) {   // n <= 25, little-endian bit order
    uint32_t v = 0;
    for (uint32_t i = 0; i < n; i++) v |= (uint32_t)((b[(pos + i) >> 3] >> ((pos + i) & 7)) & 1) << i;
    return v;
}

static uint32_t ise_bits(uint32_t count, uint32_t q) {   // q: index into {2,3,4,5,6,8,10,12,16,20,24,32}
    static const uint8_t kind[12] = {0, 1, 0, 2, 1, 0, 2, 1, 0, 2, 1, 0};   // 0 bits, 1 trits, 2 quints
    static const uint8_t nb[12] = {1, 0, 2, 0, 1, 3, 1, 2, 4, 2, 3, 5};
    if (kind[q] == 1) return (8 * count + 4) / 5 + count * nb[q];
    if (kind[q] == 2) return (7 * count + 2) / 3 + count * nb[q];
    return count * nb[q];
}

/// The 2D block mode (ASTC spec C.2.10): weight grid, dual plane, weight bits; false for a reserved mode
static bool block_mode_2d(uint32_t bm, uint32_t* weight_bits, bool* dual) {
    uint32_t base_q = (bm >> 4) & 1, H = (bm >> 9) & 1, D = (bm >> 10) & 1, A = (bm >> 5) & 3, x, y;
    if (bm & 3) {
        base_q |= (bm & 3) << 1;
        uint32_t B = (bm >> 7) & 3;
        switch ((bm >> 2) & 3) {
            case 0: x = B + 4; y = A + 2; break;
            case 1: x = B + 8; y = A + 2; break;
            case 2: x = A + 2; y = B + 8; break;
            default:
                B &= 1;
                if (bm & 0x100) { x = B + 2; y = A + 2; } else { x = A + 2; y = B + 6; }
        }
    } else {
        base_q |= ((bm >> 2) & 3) << 1;
        if (((bm >> 2) & 3) == 0) return false;
        const uint32_t B = (bm >> 9) & 3;
        switch ((bm >> 7) & 3) {
            case 0: x = 12; y = A + 2; break;
            case 1: x = A + 2; y = 12; break;
            case 2: x = A + 6; y = B + 6; D = 0; H = 0; break;
            default:
                if (((bm >> 5) & 3) == 0) { x = 6; y = 10; }
                else if (((bm >> 5) & 3) == 1) { x = 10; y = 6; }
                else return false;
        }
    }
    const uint32_t q = base_q - 2 + 6 * H, count = x * y * (D + 1);
    if (base_q < 2 || q > 11 || count > 64) return false;
    *weight_bits = ise_bits(count, q);
    *dual = D;
    return *weight_bits >= 24 && *weight_bits <= 96;
}

/// counts[0] blocks with an HDR endpoint mode (2, 3, 7, 11, 14, 15), [1] HDR void-extent blocks, [2] reserved or
/// invalid blocks, [3] LDR void-extent blocks (constant colour)
HK_API void hk_astc_scan(const uint8_t* blocks, uint64_t n, uint64_t* counts) {
    for (uint64_t i = 0; i < n; i++) {
        const uint8_t* b = blocks + i * 16;
        const uint32_t bm = bits(b, 0, 11);
        if ((bm & 0x1FF) == 0x1FC) { counts[(bm >> 9) & 1 ? 1 : 3]++; continue; }
        uint32_t wb = 0;
        bool dual = false;
        if (!block_mode_2d(bm, &wb, &dual)) { counts[2]++; continue; }
        const uint32_t parts = bits(b, 11, 2) + 1;
        if (parts == 4 && dual) { counts[2]++; continue; }
        uint32_t cem[4] = {0, 0, 0, 0};
        if (parts == 1) {
            cem[0] = bits(b, 13, 4);
        } else {
            const uint32_t low = bits(b, 23, 6);
            if ((low & 3) == 0) {
                for (uint32_t p = 0; p < parts; p++) cem[p] = (low >> 2) & 0xF;
            } else {
                const uint32_t hs = 3 * parts - 4, pos = 128 - wb - hs;
                const uint32_t enc = low | (bits(b, pos, hs) << 6);
                const uint32_t base = (enc & 3) - 1;
                uint32_t bp = 2;
                for (uint32_t p = 0; p < parts; p++) cem[p] = (base + ((enc >> bp++) & 1)) << 2;
                for (uint32_t p = 0; p < parts; p++, bp += 2) cem[p] |= (enc >> bp) & 3;
            }
        }
        bool hdr = false;
        for (uint32_t p = 0; p < parts; p++) {
            const uint32_t c = cem[p];
            hdr |= c == 2 || c == 3 || c == 7 || c == 11 || c == 14 || c == 15;
        }
        if (hdr) counts[0]++;
    }
}

// ---- encoders and decoders -------------------------------------------------------------------------------------------
// kind: 1 BC1 (opaque, no 3-colour black: index 3 would be transparent in BC1_RGBA), 4 BC4 (channel c0), 5 BC5 (c0 ->
// R, c1 -> G), 7 BC7 (bc7e; level 0..6 = ultrafast..slowest; RGBA as given). rgba: w x h x 4 bytes, packed rows. out:
// ceil(w/4) * ceil(h/4) blocks, row-major. Edge blocks repeat the last row and column. Returns 0, or -1 (no encoders).
#ifdef HK_BC7ENC
static void gather(const uint8_t* rgba, uint32_t w, uint32_t h, uint32_t bx, uint32_t by, uint8_t* px) {
    for (uint32_t y = 0; y < 4; y++)
        for (uint32_t x = 0; x < 4; x++) {
            const uint32_t sx = std::min(bx * 4 + x, w - 1), sy = std::min(by * 4 + y, h - 1);
            std::memcpy(px + (y * 4 + x) * 4, rgba + ((uint64_t)sy * w + sx) * 4, 4);
        }
}
static bool g_init = false;
static void init_once() {
    if (g_init) return;
    rgbcx::init(rgbcx::bc1_approx_mode::cBC1Ideal);
    ispc::bc7e_compress_block_init();
    g_init = true;
}
#endif

HK_API int hk_encode(int kind, uint32_t w, uint32_t h, const uint8_t* rgba, uint8_t* out, int c0, int c1, int level,
                     int radius) {
#ifdef HK_BC7ENC
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
        return 0;
    }
    uint8_t px[64];
    const uint32_t bpb = kind == 5 ? 16 : 8;
    for (uint32_t by = 0; by < bh; by++)
        for (uint32_t bx = 0; bx < bw; bx++) {
            gather(rgba, w, h, bx, by, px);
            uint8_t* o = out + ((uint64_t)by * bw + bx) * bpb;
            if (kind == 1) rgbcx::encode_bc1(level < 0 ? rgbcx::MAX_LEVEL : (uint32_t)level, o, px, true, false);
            else if (kind == 4) rgbcx::encode_bc4_hq(o, px + c0, 4, (uint32_t)radius, rgbcx::BC4_USE_ALL_MODES);
            else if (kind == 5) rgbcx::encode_bc5_hq(o, px, (uint32_t)c0, (uint32_t)c1, 4, (uint32_t)radius, rgbcx::BC4_USE_ALL_MODES);
            else return -2;
        }
    return 0;
#else
    (void)kind; (void)w; (void)h; (void)rgba; (void)out; (void)c0; (void)c1; (void)level; (void)radius;
    return -1;
#endif
}

/// Blocks -> RGBA (w x h x 4, packed rows, cropped). BC4: R, G = B = 0, A = 255; BC5: R, G, B = 0, A = 255.
HK_API int hk_decode(int kind, uint32_t w, uint32_t h, const uint8_t* blocks, uint8_t* rgba) {
#ifdef HK_BC7ENC
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
#else
    (void)kind; (void)w; (void)h; (void)blocks; (void)rgba;
    return -1;
#endif
}
