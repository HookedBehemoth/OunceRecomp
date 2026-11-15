// The per-vertex light of the game's probe grid, as its lit meshlet stage with probes reads it (mesh stage
// 40d0c0768e874600: the lit meshlet stage plus this lookup). A transcription of the game's data layout and arithmetic
// (docs/direct/task-mesh.md); our own text. Every multiply, add and fused multiply-add is in the game's
// order, so that the result is the translated stage's bit for bit.
//
// Storage slot 1 of the mesh stage (PROBE_SLOT) holds five guest pointers:
//   [0] three strides (words 0-2, each >> 4: 16-byte rows a cell, a leaf, a probe number)
//   [1] the grid: +16 origin (3 floats), +28 cell size, +32 cells in x, y, z
//   [2] the cells, 16 bytes each: first leaf, leaves a side (n), first probe
//   [3] the leaves, two 16-byte rows each: 8 probe numbers of 16 bits; an offset (3 floats) and a scale for the weights
//   [4] the probes, 16-byte rows: row 0's word 3 bit 0 says the probe is valid, rows 1-7 its coefficients
// Storage slot 0 (LIGHT_SLOT) holds the scene's own coefficients at +48..+111 (4 rows), c6 the scales.

#ifndef PROBE_SLOT
#define PROBE_SLOT 1
#endif
#ifndef LIGHT_SLOT
#define LIGHT_SLOT 0
#endif

uint slot_guest_lo(uint slot) { return c0.w[(0x770u + 16u * slot) / 4u]; }
uint64_t slot_base(uint slot) {
    return device_address(uint64_t(c0.w[(0x770u + 16u * slot) / 4u]) | uint64_t(c0.w[(0x774u + 16u * slot) / 4u]) << 32);
}
// (the word at a byte offset of a storage slot, as NVN addresses it: from the bound address, 4-byte words)
uint slot_word(uint64_t base, uint lo, uint offset) { return Words(base + uint64_t(((lo + offset) - (lo & ~3u)) & ~3u)).w[0]; }
vec4 slot_row(uint64_t base, uint lo, uint offset) {
    return vec4(uintBitsToFloat(slot_word(base, lo, offset)), uintBitsToFloat(slot_word(base, lo, offset + 4u)),
                uintBitsToFloat(slot_word(base, lo, offset + 8u)), uintBitsToFloat(slot_word(base, lo, offset + 12u)));
}
// (16 bytes at a guest address)
uvec4 guest_row(uint64_t guest) {
    Words p = Words(device_address(guest));
    return uvec4(p.w[0], p.w[1], p.w[2], p.w[3]);
}
uint guest_word(uint64_t guest) { return Words(device_address(guest)).w[0]; }

// (a float to a grid coordinate, as the game converts it: not-a-number is 0, held to [0, last])
int cell_coordinate(float q, uint count) {
    const int c = isnan(q) ? 0 : int(min(max(trunc(q), -2147483648.0), 2147483648.0));
    return min(max(0, c), int(count) - 1);
}
uint leaf_coordinate(float f, uint n) {
    const float g = isnan(f) ? 0.0 : f;
    const uint u = uint(min(max(trunc(min(g, 4294967040.0)), 0.0), 4294967296.0)) | (g >= 4294967296.0 ? ~0u : 0u);
    return min(n - 1u, u);
}

const float kSh0 = 0.282094806;   // (1 / (2 sqrt(pi)): the constant term of the spherical harmonics)

// The 12 light components of a vertex at `position` (world position plus the object's offset): three output lines
void probe_light(vec3 position, out vec4 line0, out vec4 line1, out vec4 line2) {
    const uint table_lo = slot_guest_lo(PROBE_SLOT);
    const uint64_t table = slot_base(PROBE_SLOT);
    uint64_t pointer[5];
    for (uint k = 0u; k < 5u; k++)
        pointer[k] = uint64_t(slot_word(table, table_lo, 8u * k)) | uint64_t(slot_word(table, table_lo, 8u * k + 4u)) << 32;
    const uint cell_stride = guest_word(pointer[0]) >> 4, leaf_stride = guest_word(pointer[0] + 4ul) >> 4;
    const uint probe_stride = guest_word(pointer[0] + 8ul) >> 4;
    const uvec4 grid = guest_row(pointer[1] + 16ul);          // (origin, cell size)
    const uvec3 cells = guest_row(pointer[1] + 32ul).xyz;

    // (the cell: the position in cells, held to the grid)
    precise float per_cell = 1.0 / uintBitsToFloat(grid.w);
    precise vec3 q = (position - uintBitsToFloat(grid.xyz)) * per_cell;
    const ivec3 c = ivec3(cell_coordinate(q.x, cells.x), cell_coordinate(q.y, cells.y), cell_coordinate(q.z, cells.z));
    const uint cell_index = uint((c.z * int(cells.y) + c.y) * int(cells.x) + c.x) * cell_stride;
    const uvec4 cell = guest_row(pointer[2] + uint64_t(cell_index) * 16ul);   // (first leaf, n, first probe)
    const uint n = cell.y;

    // (the leaf: the position within the cell in leaves, held to the cell's n)
    precise vec3 f = min(max(q - vec3(uvec3(c)), 0.0), 1.0) * float(n);
    const uvec3 d = uvec3(leaf_coordinate(f.x, n), leaf_coordinate(f.y, n), leaf_coordinate(f.z, n));
    const uint leaf = (cell.x + (n * (n * d.z + d.y) + d.x)) * leaf_stride;
    const uvec4 numbers = guest_row(pointer[3] + uint64_t(leaf) * 16ul);
    const uvec4 weights = guest_row(pointer[3] + uint64_t(leaf + 1u) * 16ul);   // (offset, scale)

    // (trilinear weights of the leaf's 8 probes; a probe that is not valid counts 1e-5)
    precise vec3 t = fma(min(max(f - vec3(d), 0.0), 1.0), vec3(uintBitsToFloat(weights.w)), uintBitsToFloat(weights.xyz));
    precise vec3 s = (-t) + 1.0;
    precise float xy[4];
    xy[0] = s.x * s.y, xy[1] = t.x * s.y, xy[2] = t.y * s.x, xy[3] = t.x * t.y;
    uint64_t probe[8];
    precise float w[8];
    for (uint i = 0u; i < 8u; i++) {
        const uint number = (numbers[i >> 1] >> (16u * (i & 1u))) & 0xffffu;
        probe[i] = device_address(pointer[4] + uint64_t((cell.z + number) * probe_stride) * 16ul);
        const float valid = (Words(probe[i]).w[3] & 1u) != 0u ? 1.0 : uintBitsToFloat(0x3727c5acu);
        precise float corner = (i < 4u ? s.z : t.z) * xy[i & 3u];
        w[i] = corner * valid;
    }
    precise float sum = w[7] + ((((((w[0] + w[1]) + w[2]) + w[3]) + w[4]) + w[5]) + w[6]);
    precise float normalise = 1.0 / sum;

    // (rows 1-7 of the 8 probes, weighted)
    precise vec4 row[7];
    for (uint r = 0u; r < 7u; r++) {
        precise vec4 acc = vec4(0.0);
        for (uint i = 0u; i < 8u; i++) {
            Words p = Words(probe[i] + uint64_t(16u * (r + 1u)));
            const vec4 v = vec4(uintBitsToFloat(p.w[0]), uintBitsToFloat(p.w[1]), uintBitsToFloat(p.w[2]), uintBitsToFloat(p.w[3]));
            if (i == 0u) acc = v * w[0]; else acc = fma(v, vec4(w[i]), acc);
        }
        row[r] = normalise * acc;
    }

    // (mixed with the scene's own coefficients)
    const uint light_lo = slot_guest_lo(LIGHT_SLOT);
    const uint64_t light = slot_base(LIGHT_SLOT);
    const vec3 l0 = slot_row(light, light_lo, 48u).xyz, l1 = slot_row(light, light_lo, 64u).xyz;
    const vec3 l2 = slot_row(light, light_lo, 80u).xyz, l3 = slot_row(light, light_lo, 96u).xyz;
    precise float scale = c6.f[137] * kSh0;
    precise vec3 ambient_scale = c6.f[136] * vec3(c6.f[20], c6.f[21], c6.f[22]);
    precise vec3 ambient = fma(l0, vec3(scale), ambient_scale);
    const float mix_factor = c6.f[225];
    precise float o[12];
    for (uint j = 0u; j < 12u; j++) {
        const uint g = j / 3u, k = j % 3u;
        precise float own;
        if (g == 0u) {
            precise float a = row[1].w * l1[k];
            own = fma(row[3].w, l3[k], fma(row[2].w, l2[k], fma(row[0].w, l0[k], a)));
        } else {
            const float lg = g == 1u ? l1[k] : g == 2u ? l2[k] : l3[k];
            precise float a = row[g].w * l0[k];
            own = fma(row[0].w, lg, a);
        }
        precise float own_scaled = own * kSh0;
        o[j] = fma(row[4u + j / 4u][j % 4u], ambient[k], fma(row[g][k], mix_factor, own_scaled));
    }
    line0 = vec4(o[0], o[1], o[2], o[3]);
    line1 = vec4(o[4], o[5], o[6], o[7]);
    line2 = vec4(o[8], o[9], o[10], o[11]);
}
