#version 460
// The full-screen composite of the main view's atmosphere and volumetric fog (game fragment stage 2407c9a14d62a1d6: Kokiri's and
// Hyrule Field's 1536x864 post pass, one draw of 3 vertices). Decompiled from the translator's reading of the game's code
// (docs/direct/shader-costs.md, part 3); not used by the game. What it does, as far as the arithmetic tells:
// - reads the scene depth (c2+0x50) and a depth gather (c2+0x58) at the pixel, and a second depth-like value (c2+0x38) whose
//   reciprocal against c3 gives a view distance; decides "sky or past the fog's far plane" (`far`);
// - shifts the pixel's texture coordinate by up to one texel along the depth gradient (an edge-aware offset) and reads two
//   layers there (c2+0x60, c2+0x68), and the scene colour at the pixel (c2+0x70);
// - for the pixels not marked far by the second depth test: the sky/atmosphere colour from a 3D table (c2+0x30) addressed by
//   the view ray and a logarithmic distance slice, and either that or a scattering table (c2+0x48) addressed by the sun angle
//   and height (one of two paths chosen by c4+0x658), tone-compressed past a luminance of 5 with an arctangent;
// - the volumetric fog: two 3D lookups (c2+0x20, c2+0x28) at the pixel's froxel (logarithmic depth slice) give
//   in-scattering and extinction, blended into the colour; the result composed with the two shifted layers.
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

BANK(c2, 0);
BANK(c3, 1);
BANK(c4, 2);
layout(set = 0, binding = BINDING_BASE + 3) uniform texture3D tex_20[];
layout(set = 0, binding = BINDING_BASE + 4) uniform texture3D tex_28[];
layout(set = 0, binding = BINDING_BASE + 5) uniform texture3D tex_30[];
layout(set = 0, binding = BINDING_BASE + 6) uniform texture2D tex_38[];
layout(set = 0, binding = BINDING_BASE + 7) uniform texture2D tex_40[];
layout(set = 0, binding = BINDING_BASE + 8) uniform texture2D tex_48[];
layout(set = 0, binding = BINDING_BASE + 9) uniform texture2D tex_50[];
layout(set = 0, binding = BINDING_BASE + 10) uniform texture2D tex_58[];
layout(set = 0, binding = BINDING_BASE + 11) uniform texture2D tex_60[];
layout(set = 0, binding = BINDING_BASE + 12) uniform texture2D tex_68[];
layout(set = 0, binding = BINDING_BASE + 13) uniform texture2D tex_70[];

layout(location = 0) noperspective in vec4 in0;   // (.xy: the pixel in texels of the depth target)
layout(location = 1) noperspective in vec4 in1;   // (.xy: the screen texture coordinate)
layout(location = 0) out vec4 out0;

#define S2(t, off, uv) texture(sampler2D(t[IMAGE(U(c2, off))], SAMPLER(U(c2, off))), uv)
#define S3(t, off, uvw) texture(sampler3D(t[IMAGE(U(c2, off))], SAMPLER(U(c2, off))), uvw)

/// The scattering table's coordinate of a cosine: acos(c) / (pi / 2) by the game's polynomial (sqrt(1 - c) * poly(c))
precise float acos_coord(float c) {
    precise float r = fma(sqrt(-c + 1.0) * 0.636619747, -fma(c, fma(c, fma(c, -0.0187292993, 0.0742609948), -0.212114394), 1.57072878), 1.0);
    return r;
}

void main() {
    precise vec2 uv = in1.xy;
    precise float depth = S2(tex_50, 0x50, uv).x;
    precise vec4 gather = textureGather(sampler2D(tex_58[IMAGE(U(c2, 0x58))], SAMPLER(U(c2, 0x58))), uv, 0);
    precise float fx = in0.x + -floor(in0.x), fy = in0.y + -floor(in0.y);
    precise float view_z = fma(depth, F(c3, 2536), F(c3, 2512));
    precise float far_z = (rcp(fma(S2(tex_38, 0x38, uv).x, F(c3, 3028), F(c3, 3024)))) * -F(c3, 3056);
    const bool sky = depth > 0.999899983;
    const bool far = sky || (view_z <= -far_z && !sky);
    // (the depth-gradient offset: up to one texel toward the steeper side)
    precise float scale = (rcp((depth + F(c3, 2540)) * F(c3, 2564))) * 2.0;
    precise float gy = -abs(-depth + gather.y) + abs(-depth + gather.w);
    precise float gx = abs(-depth + gather.x) + -abs(-depth + gather.z);
    precise float other = 0.0;
    if (!far) other = S2(tex_40, 0x40, uv).x;
    precise float ox = fma(-fx + sat(fma(scale, gy + gx, fx)), F(c3, 296), uv.x);
    precise float oy = fma(-fy + sat(fma(scale, gy + -gx, fy)), F(c3, 300), uv.y);
    precise vec4 scene = S2(tex_70, 0x70, uv);
    precise vec4 layer_a = S2(tex_60, 0x60, vec2(ox, oy));
    precise vec4 layer_b = S2(tex_68, 0x68, vec2(ox, oy));
    bool atmosphere = depth <= 0.999899983;
    if (!far) atmosphere = !(other > 0.0) && atmosphere;

    precise float r = scene.x, g = scene.y, b = scene.z;
    if (atmosphere) {
        precise float log_near = log2(F(c4, 1520));
        precise float ry = (fma(uv.y, 2.0, -1.0) * F(c3, 276));
        precise float rx = (fma(uv.x, 2.0, -1.0) * F(c3, 272));
        precise float vy = view_z * ry, vx = view_z * rx;
        precise float inv_len = inversesqrt(fma(rx, rx, ry * ry) + 1.0);
        precise float dist = sqrt(fma(view_z, view_z, fma(vx, vx, vy * vy)));
        precise float neg_dist = dist * -inv_len;   // (stored as its negation: -dist is the distance used below)
        const bool table_path = 0.0 != F(c4, 1624) || isnan(F(c4, 1624));
        precise float sun_x = fma(F(c4, 1512), F(c3, 24), fma(F(c4, 1504), F(c3, 16), F(c4, 1508) * F(c3, 20)));
        precise float sun_y = fma(F(c4, 1512), F(c3, 40), fma(F(c4, 1504), F(c3, 32), F(c4, 1508) * F(c3, 36)));
        precise float sun_z = fma(F(c4, 1512), F(c3, 8), fma(F(c4, 1504), F(c3, 0), F(c4, 1508) * F(c3, 4)));
        precise float dx = rx * inv_len, dy = ry * inv_len;
        precise float k = rcp((-log_near + log2(F(c4, 1524))) * 0.693147182);
        precise vec3 sky_at = vec3(uv.x * F(c4, 1536), uv.y * F(c4, 1540),
                                   exp2(log2(abs((-log_near + log2(min(max(-neg_dist, F(c4, 1520)), F(c4, 1524)))) * 0.693147182 * k)) * (rcp(F(c4, 1528)))) *
                                       F(c4, 1544));
        precise float sky_lum = sat((S3(tex_30, 0x30, sky_at).x + max(0.0, -neg_dist + -F(c4, 1524))) * (rcp(-neg_dist)));
        precise float p58 = sun_z, p59 = sun_x, p60 = sun_y, p61 = dx, p62 = dy, p63 = sky_lum, p64 = inv_len, p65 = dist;
        if (!table_path) {
            precise float h = log2(abs(-sat(fma(dist, F(c4, 1600), -F(c4, 1604))) + 1.0));
            precise float mu = sat(fma(fma(sun_y, -inv_len, fma(sun_z, dx, sun_x * -dy)), -0.5, 0.5));
            precise vec2 scat_at = vec2(acos_coord(mu), fma(fma(exp2(h * F(c4, 1636)), -F(c4, 1644) + 1.0, F(c4, 1644)), 0.5, 0.5));
            precise vec4 scat = S2(tex_48, 0x48, scat_at);
            precise float up = exp2(log2(abs(sat(dy))) * F(c4, 1656));
            precise float fade = sat((-exp2(h * F(c4, 1632)) + 1.0) * F(c4, 1640));
            precise float haze = sat(((-exp2(log2(abs(-sat(fma(dist, F(c4, 1616), -F(c4, 1620))) + 1.0)) * F(c4, 1648)) + 1.0) * fma(-up, F(c4, 1660), 1.0)) *
                                   F(c4, 1676));
            precise float keep = -haze + 1.0;
            precise float hz_g = haze * F(c4, 1668), w = fade * keep, hz_b = haze * F(c4, 1672);
            p58 = up;
            p59 = hz_b;
            p60 = scat.z;
            p61 = hz_g;
            p62 = fma(scat.x, w, haze * F(c4, 1664));
            p63 = fma(scat.z, w, hz_b);
            p64 = fma(scat.y, w, hz_g);
            p65 = sky_lum * fma(-(-fade + 1.0), keep, 1.0);
        }
        precise float a113 = p65, a114 = p63, a115 = p64, a116 = p62;
        if (table_path) {
            precise float lx = fma(-p59 + -F(c3, 20), F(c4, 1608), p59);
            precise float ly = fma(-p58 + -F(c3, 4), F(c4, 1608), p58);
            precise float lz = fma(-p60 + -F(c3, 36), F(c4, 1608), p60);
            precise float il = inversesqrt(fma(lz, lz, fma(ly, ly, lx * lx)));
            precise float mu = fma(-p64, lz * il, fma(p61, ly * il, p62 * -(lx * il)));
            precise float m = sat(fma(mu, -0.5, 0.5));
            precise vec2 scat_at = vec2(acos_coord(m), fma(exp2(log2(abs(fma(sat(fma(-p64, F(c3, 200), fma(p61, F(c3, 192), p62 * -F(c3, 196)))), 0.5, 0.5))) *
                                                            F(c4, 1636)),
                                                       0.5, 0.5));
            precise vec4 scat = S2(tex_48, 0x48, scat_at);
            precise float inv_a = rcp(F(c4, 1600));
            precise float bb = (1.0 + F(c4, 1604)) * inv_a;
            precise float depth_k = ((bb * exp2(exp2(log2(abs((rcp(max(F(c4, 1632), 0.100000001))) * 30.0)) * 0.5) *
                                             log2(abs(max(0.0, fma(-inv_a, F(c4, 1604), p65)) * (rcp(bb)))))) *
                                   fma(fma(mu, 0.5, 0.5), -1.0 + F(c4, 1612), 1.0)) *
                                  (F(c4, 1640) * 1.17647052);
            precise float ext = exp2(depth_k * -0.000216404267);
            precise float ins = (-exp2((p63 * (depth_k * -0.000150000007)) * 1.44269502) + 1.0) * 1.33333325;
            precise float norm = rcp(min(max(-ext + 1.0, 9.99999975e-05), 1.0));
            precise float haze = p63 * sat(((-exp2(log2(abs(-sat(fma(p65, F(c4, 1616), -F(c4, 1620))) + 1.0)) * F(c4, 1648)) + 1.0) *
                                          fma(-exp2(log2(abs(sat(p62))) * F(c4, 1656)), F(c4, 1660), 1.0)) *
                                         F(c4, 1676));
            precise float w = norm * ins;
            precise float sg = scat.y * w, sr = scat.x * w, sb = (scat.z * ins) * norm;
            precise float cg = fma(haze, -sg + F(c4, 1668), sg);
            precise float cr = fma(haze, -sr + F(c4, 1664), sr);
            precise float cb = fma(haze, -sb + F(c4, 1672), sb);
            precise float part = fma(cr, 0.298911989, cg * 0.586610973);
            precise float lum = fma(cb, 0.114478, part);
            precise float tr = cr, tb = cb, tg = cg;
            if (lum > 5.0) {   // (past 5: compressed to 5 + atan(lum - 5), by the game's arctangent polynomial)
                precise float t = lum + -5.0;
                precise float at = abs(t);
                const bool big = at > 1.0;
                precise float x = big ? rcp(at) : -0.0 + at;
                precise float x2 = x * x;
                precise float poly = fma((x2 * fma(x2, fma(x2, -0.823362947, -5.67486715), -6.5655551)) * x,
                                       rcp(fma(x2, fma(x2, x2 + 11.3353882, 28.8424683), 19.6966705)), x);
                precise float y = big ? -poly + 1.57079637 : poly;
                if (!isnan(at)) y = uintBitsToFloat((floatBitsToUint(t) & 0x80000000u) | floatBitsToUint(y));
                precise float s = (y + 5.0) * (rcp(lum));
                tr = cr * s, tb = cb * s, tg = cg * s;
            }
            a113 = fma(-ext, -haze + 1.0, 1.0);
            a114 = tb, a115 = tg, a116 = tr;
        }
        b = fma(-scene.z + a114, a113, scene.z);
        g = fma(-scene.y + a115, a113, scene.y);
        r = fma(-scene.x + a116, a113, scene.x);
    }
    // (the volumetric fog at the pixel's froxel)
    precise float x129 = uv.x, x130 = uv.y, x131 = view_z, x132 = far_z, x133 = b, x134 = r;
    if (far) {
        precise float log_near = log2(F(c4, 1520));
        precise float fy2 = sat(uv.y) * F(c4, 1540), fx2 = sat(uv.x) * F(c4, 1536);
        precise float z = exp2(log2(abs((-log_near + log2(min(max(view_z, F(c4, 1520)), F(c4, 1524)))) * 0.693147182 *
                                      (rcp((-log_near + log2(F(c4, 1524))) * 0.693147182)))) *
                             (rcp(F(c4, 1528)))) *
                        F(c4, 1544);
        precise vec4 ext = texture(sampler3D(tex_28[IMAGE(U(c2, 0x28))], SAMPLER(U(c2, 0x28))), vec3(fx2, fy2, z));
        precise vec4 ins = texture(sampler3D(tex_20[IMAGE(U(c2, 0x20))], SAMPLER(U(c2, 0x20))), vec3(fx2, fy2, z));
        precise float f = sat(view_z * (rcp(F(c4, 1520))));
        precise float ig = ins.y * f, ib = ins.z * f;
        x129 = ig, x130 = ib, x131 = f;
        x132 = fma(exp2(-(ext.x * f)), r, ins.x * f);
        x133 = fma(exp2(-(ext.z * f)), b, ib);
        x134 = fma(exp2(-(ext.y * f)), g, ig);
    }
    precise float fr = x132, fg = x134, fb = x133;
    if (!far) {
        precise float log_near = log2(F(c4, 1520));
        precise float fx2 = sat(x129) * F(c4, 1536);
        precise float inv_n = rcp(F(c4, 1528));
        precise float k = (rcp((-log_near + log2(F(c4, 1524))) * 0.693147182)) * 0.693147182;
        precise float fy2 = sat(fma(F(c4, 556) * 0.013888889, rcp(F(c4, 1540)), x130)) * F(c4, 1540);
        precise float z0 = exp2(inv_n * log2(abs((-log_near + log2(min(max(x131, F(c4, 1520)), F(c4, 1524)))) * k))) * F(c4, 1544);
        precise vec4 e0 = texture(sampler3D(tex_28[IMAGE(U(c2, 0x28))], SAMPLER(U(c2, 0x28))), vec3(fx2, fy2, z0));
        precise float z1 = exp2(log2(abs((-log_near + log2(min(max(-x132, F(c4, 1520)), F(c4, 1524)))) * k)) * inv_n) * F(c4, 1544);
        precise vec4 e1 = texture(sampler3D(tex_28[IMAGE(U(c2, 0x28))], SAMPLER(U(c2, 0x28))), vec3(fx2, fy2, z1));
        precise vec4 i1 = texture(sampler3D(tex_20[IMAGE(U(c2, 0x20))], SAMPLER(U(c2, 0x20))), vec3(fx2, fy2, z1));
        precise vec4 i0 = texture(sampler3D(tex_20[IMAGE(U(c2, 0x20))], SAMPLER(U(c2, 0x20))), vec3(fx2, fy2, z0));
        precise float inv_near = rcp(F(c4, 1520));
        precise float f0 = sat(x131 * inv_near);
        precise float f1 = sat(x132 * -inv_near);
        fb = fma(exp2(fma(e1.z, f1, -(f0 * e0.z))), x133, fma(f0, i0.z, -(i1.z * f1)));
        fr = fma(exp2(fma(e1.x, f1, -(f0 * e0.x))), x134, fma(f0, i0.x, -(i1.x * f1)));
        fg = fma(exp2(fma(e1.y, f1, -(f0 * e0.y))), g, fma(f0, i0.y, -(i1.y * f1)));
    }
    precise float a = far ? layer_a.w : 1.0;
    precise vec4 o;
    o.x = fma(a, fma(layer_b.w, fr, layer_b.x), far ? layer_a.x : 0.0);
    o.y = fma(a, fma(layer_b.w, fg, layer_b.y), far ? layer_a.y : 0.0);
    o.z = fma(a, fma(layer_b.w, fb, layer_b.z), far ? layer_a.z : 0.0);
    o.w = 1.0;
    out0 = vec4(guard(o.x), guard(o.y), guard(o.z), guard(o.w));
}
