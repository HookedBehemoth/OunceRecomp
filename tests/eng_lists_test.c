/* host/eng_lists.c built as it is (tests/test_eng_lists.py): recordings made by hand, their packets checked */
#include "../host/eng_lists.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define NEED(c) do { if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

typedef struct { uint8_t b[4096]; size_t n; } rec;
static void *put(rec *r, uint32_t op, size_t payload) {
    const uint32_t sz = (uint32_t)((8 + payload + 7) & ~(size_t)7);
    nvn_op_hdr h = {op, sz};
    memcpy(r->b + r->n, &h, 8);
    memset(r->b + r->n + 8, 0, sz - 8);
    void *p = r->b + r->n + 8;
    r->n += sz;
    return p;
}
static void ubo(rec *r, int ssbo, int stage, int index, uint64_t addr, uint64_t size) {
    nvn_p_bind_buf *b = (nvn_p_bind_buf *)put(r, ssbo ? NOP_BIND_SSBO : NOP_BIND_UBO, sizeof *b);
    b->stage = stage, b->index = index, b->addr = addr, b->size = size;
}
static void tex(rec *r, int stage, int first, int n, const uint64_t *h, uint64_t src) {
    nvn_p_bind_handles *b = (nvn_p_bind_handles *)put(r, NOP_BIND_TEX, offsetof(nvn_p_bind_handles, h) + 8 * (size_t)n);
    b->stage = stage, b->first = first, b->n = n, b->src = src;
    memcpy(b->h, h, 8 * (size_t)n);
}
static void vertex(rec *r, uint64_t vb) {
    nvn_p_bind_vattr *a = (nvn_p_bind_vattr *)put(r, NOP_BIND_VATTR, offsetof(nvn_p_bind_vattr, st) + 8);
    a->n = 2, a->st[0] = 0x29, a->st[1] = 0x1069;
    nvn_p_bind_vstream *s = (nvn_p_bind_vstream *)put(r, NOP_BIND_VSTREAM, offsetof(nvn_p_bind_vstream, st) + 8);
    s->first = 0, s->n = 1, s->st[0].stride = 0x20;
    nvn_p_bind_vbuf *v = (nvn_p_bind_vbuf *)put(r, NOP_BIND_VBUF, offsetof(nvn_p_bind_vbuf, r) + 16);
    v->first = 0, v->n = 1, v->r[0].address = vb, v->r[0].size = 0x1e0;
}

int main(void) {
    eng_lists_init(1, NULL, NULL, NULL);
    NEED(eng_lists_kind(0x804582acull) == NVN_PK_VERTEX && eng_lists_kind(0x806006d8ull) == NVN_PK_SHAPE);
    NEED(eng_lists_kind(0x80293b14ull) == NVN_PK_DRAW && eng_lists_kind(0x12345678ull) == NVN_PK_NONE);

    /* a vertex list */
    rec v = {.n = 0};
    put(&v, NOP_BEGIN, 0);
    vertex(&v, 0x7fd3995100ull);
    put(&v, NOP_END, 0);
    nvn_pkt *pv = eng_lists_build(NVN_PK_VERTEX, v.b, v.n, NULL);
    NEED(pv && pv->vtx && (pv->flags & NVN_PKF_VATTR) && pv->vtx->nvattr == 2 && pv->vtx->vattr[1] == 0x1069);
    NEED(pv && pv->vtx->vstream_mask == 1 && pv->vtx->vbuf_mask == 1 && pv->vtx->vbuf[0].address == 0x7fd3995100ull);
    eng_lists_set_handle(pv, 0x1111);

    /* a shape list: buffers (one bound twice: the last wins), a texture, then the vertex list called (NOP_PKT + its records) */
    rec s = {.n = 0};
    put(&s, NOP_BEGIN, 0);
    ubo(&s, 0, 1, 2, 0x117881ca00ull, 0x3d0);
    ubo(&s, 0, 0, 1, 0x1000, 0x100);
    ubo(&s, 1, 0, 0, 0x11371e7f00ull, 0xc0);
    ubo(&s, 0, 0, 1, 0x117881c500ull, 0x100);
    const uint64_t h1 = 0x189002beull;
    tex(&s, 1, 3, 1, &h1, 0);
    nvn_p_pkt *np = (nvn_p_pkt *)put(&s, NOP_PKT, sizeof *np);
    np->pkt = pv, np->serial = pv->serial;
    const size_t at = s.n;
    vertex(&s, 0x9999);   /* (what the default path copies in: skipped, the packet stands for it) */
    np = (nvn_p_pkt *)(s.b + at - sizeof *np);
    np->skip = (uint32_t)(s.n - at);
    put(&s, NOP_END, 0);
    const uint64_t meta[4] = {1, 2, 1, 0};
    nvn_pkt *ps = eng_lists_build(NVN_PK_SHAPE, s.b, s.n, meta);
    NEED(ps && ps->nbuf == 3 && ps->nhandle == 1 && ps->nref == 1 && ps->ref[0].pkt == pv && ps->meta[2] == 1);
    NEED(ps && ps->buf[1].b.addr == 0x117881c500ull && ps->buf[2].ssbo == 1);
    NEED(ps && ps->hnd[0].stage == 1 && ps->hnd[0].slot == 3 && ps->hnd[0].h == h1 && (ps->flags & NVN_PKF_TEX));
    NEED(ps && ps->vtx && ps->vtx->vbuf[0].address == 0x7fd3995100ull);   /* (folded from the packet, not the records after it) */

    /* the same with the vertex list gone: its records decode */
    eng_lists_released(pv);
    nvn_p_pkt live = {pv, np->serial, 0};
    NEED(nvn_pkt_live(&live) == NULL);
    nvn_pkt *ps2 = eng_lists_build(NVN_PK_SHAPE, s.b, s.n, meta);
    NEED(ps2 && ps2->nref == 0 && ps2->vtx && ps2->vtx->vbuf[0].address == 0x9999);

    /* a draw list; refusals */
    rec d = {.n = 0};
    put(&d, NOP_BEGIN, 0);
    nvn_p_draw_idx *di = (nvn_p_draw_idx *)put(&d, NOP_DRAW_IDX, sizeof *di);
    di->prim = 4, di->itype = 1, di->count = 0x4c2, di->instances = 1, di->ibuf = 0x7fb8955000ull;
    put(&d, NOP_END, 0);
    nvn_pkt *pd = eng_lists_build(NVN_PK_DRAW, d.b, d.n, NULL);
    NEED(pd && pd->draw.op == NOP_DRAW_IDX && pd->draw.u.di.count == 0x4c2 && pd->draw.u.di.ibuf == 0x7fb8955000ull && !pd->vtx);
    rec bad = {.n = 0};
    put(&bad, NOP_BEGIN, 0);
    put(&bad, NOP_DRAW_IDX, sizeof(nvn_p_draw_idx));
    ubo(&bad, 0, 0, 1, 1, 1);
    put(&bad, NOP_END, 0);
    NEED(eng_lists_build(NVN_PK_DRAW, bad.b, bad.n, NULL) == NULL);
    rec def = {.n = 0};
    put(&def, NOP_BEGIN, 0);
    tex(&def, 1, 0, 1, &h1, 0x1000);
    put(&def, NOP_END, 0);
    NEED(eng_lists_build(NVN_PK_SHAPE, def.b, def.n, NULL) == NULL);
    rec other = {.n = 0};
    put(&other, NOP_BEGIN, 0);
    put(&other, NOP_SET_RT, sizeof(nvn_p_set_rt));
    put(&other, NOP_END, 0);
    NEED(eng_lists_build(NVN_PK_SHAPE, other.b, other.n, NULL) == NULL);
    eng_lists_stats st;
    eng_lists_get_stats(&st);
    NEED(st.why[ENG_WHY_AFTER_DRAW] == 1 && st.why[ENG_WHY_DEFERRED] == 1 && st.why[NOP_SET_RT] == 1);

    /* lifetime: a freed packet's memory is reused with a new serial; the old reference stays stale */
    const uint32_t old = ps2->serial;
    eng_lists_released(ps2);
    eng_lists_freed(ps2);
    nvn_pkt *again = eng_lists_build(NVN_PK_SHAPE, s.b, s.n, meta);
    NEED(again == ps2 && again->serial && again->serial != old);
    nvn_p_pkt stale = {ps2, old, 0};
    NEED(nvn_pkt_live(&stale) == NULL);
    eng_lists_get_stats(&st);
    NEED(st.freed == 1 && st.released == 2);
    NEED(eng_lists_called(pd) == 1 && eng_lists_called(pv) == 0);   /* (pv was released) */

    if (fails) return 1;
    printf("PASS: packets of vertex, shape and draw lists; folded calls; refusals; released and reused packets\n");
    return 0;
}
