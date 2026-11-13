// The game's meshlet format, as its meshlet mesh stages read it (task stage 2c56dc1006942f74's mesh stages): shared by the
// depth-only and the lit stages. A transcription of the game's data layout; our own text.
//
// A model's buffer (a storage buffer of the mesh stage, slot MODEL_SLOT of the driver bank's table):
//   byte 0   origin (3 floats), 12 scale (a float): position = float(quantised) * scale + origin
//   byte 16  format flags: bits 10-11 and 12-16 each add a quadword to every meshlet record
//   byte 84  the meshlet records (4 quadwords and the optional ones each), then the meshlets' packed streams
// A record (words from its start): w0-w1 the quantised position origin (three 21-bit fields), w4 the counts (primitives in
// bits 16-23, vertices in 24-31), w5 its streams' first byte (bits 4-31, from byte 84) and the normal width (bits 0-3), w6 bits
// a vertex (0-9) and the position widths (16-20, 21-25, 26-30), w7 the tangent width (bits 0-3), w8 the texture coordinates'
// integer origin (two signed 16-bit halves), w9 their exponents (0-11, 12-23, biased by 16) and widths (24-27, 28-31).
// Each meshlet's data: a vertex stream (a record of vertex_bits a vertex: position x, y, z, then the optional normal, tangent
// and texture coordinates), then an index stream (three vertex numbers a triangle, each just wide enough for the vertices).

#ifndef MODEL_SLOT
#define MODEL_SLOT 0
#endif

layout(set = 0, binding = 0, scalar) uniform DriverBank { uint w[16384]; } c0;

uint model_guest_lo() { return c0.w[(0x770u + 16u * MODEL_SLOT) / 4u]; }
uint64_t model_base() {
    const uint64_t g = uint64_t(c0.w[(0x770u + 16u * MODEL_SLOT) / 4u]) | uint64_t(c0.w[(0x774u + 16u * MODEL_SLOT) / 4u]) << 32;
    return device_address(g);
}
// The word at a byte offset of the model's buffer (as NVN addresses it: from the bound address, 4-byte words)
uint model_word(uint64_t base, uint offset) {
    const uint lo = model_guest_lo();
    return Words(base + uint64_t(((lo + offset) - (lo & ~3u)) & ~3u)).w[0];
}
const uint kRecords = 84u;

struct Meshlet {
    uint vertices, primitives;
    uint data_bit, vertex_bits;
    uint x_bits, y_bits, z_bits;
    uvec3 base;
    uint index_bits;
    uint normal_bits, tangent_bits;   // (each of the two octahedral components)
    uint w8, w9;                      // (the texture coordinates' origin, exponents and widths)
};
Meshlet read_meshlet(uint64_t m, uint id) {
    const uint flags = model_word(m, 16u);
    const uint record_bytes = 8u * (4u + bitCount(flags & 0xc00u) + bitCount(flags & 0x1f000u));
    const uint r = kRecords + id * record_bytes;
    const uint w0 = model_word(m, r), w1 = model_word(m, r + 4u), w4 = model_word(m, r + 16u), w5 = model_word(m, r + 20u);
    const uint w6 = model_word(m, r + 24u), w7 = model_word(m, r + 28u);
    Meshlet s;
    s.primitives = bitfieldExtract(w4, 16, 8);
    s.vertices = bitfieldExtract(w4, 24, 8);
    s.data_bit = (w5 >> 4) * 8u;
    s.vertex_bits = w6 & 1023u;
    s.x_bits = bitfieldExtract(w6, 16, 5), s.y_bits = bitfieldExtract(w6, 21, 5), s.z_bits = bitfieldExtract(w6, 26, 5);
    s.base = uvec3(w0 & 0x1fffffu, ((w0 >> 21) | (w1 << 11)) & 0x1fffffu, (w1 >> 10) & 0x1fffffu);
    s.index_bits = uint(findMSB(s.vertices - 1u) + 1);
    s.normal_bits = (w5 & 15u) + 6u;
    s.tangent_bits = (w7 & 15u) + 6u;
    s.w8 = model_word(m, r + 32u), s.w9 = model_word(m, r + 36u);
    return s;
}

// (count bits from a stream's bit, count < 32 and within the 64 bits from its word)
uint low_bits(uint count) { return count >= 32u ? ~0u : (1u << count) - 1u; }
uint read_bits(uint64_t m, uint bit, uint count) {
    const uint at = kRecords + (bit >> 5) * 4u;
    const uint64_t pair = uint64_t(model_word(m, at)) | uint64_t(model_word(m, at + 4u)) << 32;
    return uint(pair >> (bit & 31u)) & low_bits(count);
}

// The vertex stream's position fields: model-space position
vec3 read_position(uint64_t m, Meshlet s, uint v) {
    const uint bit = s.data_bit + v * s.vertex_bits;
    const uint at = kRecords + (bit >> 5) * 4u;
    const uint w0 = model_word(m, at), w1 = model_word(m, at + 4u), w2 = model_word(m, at + 8u);
    const uint first = bit & 31u;
    const uvec3 q = uvec3(stream_bits(w0, w1, w2, first, s.x_bits), stream_bits(w0, w1, w2, first + s.x_bits, s.y_bits),
                          stream_bits(w0, w1, w2, first + s.x_bits + s.y_bits, s.z_bits));
    const float scale = uintBitsToFloat(model_word(m, 12u));
    const vec3 origin = vec3(uintBitsToFloat(model_word(m, 0u)), uintBitsToFloat(model_word(m, 4u)), uintBitsToFloat(model_word(m, 8u)));
    return fma(vec3(q + s.base), vec3(scale), origin);
}

// An octahedral unit vector of two `bits`-bit components (as the game decodes it: [0, 2^bits) to [-1, 1] by 2 * (q / 2^bits) *
// 32/31 - 1, z the rest, folded over where z < 0), not yet normalised
vec3 octahedral(uint packed, uint bits) {
    const float step = (1.0 / float(int(1u << bits))) * uintBitsToFloat(0x3f842108u);   // (32 / 31)
    const uint mask = low_bits(bits);
    float x = fma(float(packed & mask) * step, 2.0, -1.0);
    float y = fma(float((packed >> bits) & mask) * step, 2.0, -1.0);
    const float z = (-abs(y)) + ((-abs(x)) + 1.0);
    if ((-abs(x)) + 1.0 < abs(y)) {   // (z < 0)
        const float fx = x >= 0.0 ? 1.0 : -1.0, fy = y >= 0.0 ? 1.0 : -1.0;
        const float nx = (x >= 0.0 ? -abs(y) : abs(y)) + fx;
        const float ny = (y >= 0.0 ? -abs(x) : abs(x)) + fy;
        x = nx, y = ny;
    }
    return vec3(x, y, z);
}
// A vector scaled to unit length as the game does it (y*y first, then fused x and z)
vec3 unit(vec3 v) {
    precise float yy = v.y * v.y;
    const float r = inversesqrt(fma(v.z, v.z, fma(v.x, v.x, yy)));
    return vec3(v.x * r, v.y * r, v.z * r);
}
