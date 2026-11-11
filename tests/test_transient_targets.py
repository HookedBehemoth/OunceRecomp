"""OoT's transient render targets (native-vk step 4): what a resolved texture is checked by, a moved target taking over the
image it is made over, the creation clear left to a pass's load op (opt-in), and the GPU's unswizzle for every format and size (opt-in).

The game re-initialises ~42 targets a frame in one 512 MB virtual pool, at offsets that move with the frame's mix of passes.
Each dead one used to bump the global destroy generation, which made every draw look all of its textures up again; a texture
is now checked by its own id. A miss (nothing resolved, or a new ASTC texture left for a later frame by upload_defer) must not
stand until some texture dies: it is retried when a frame left textures for later, or the deferred texture would stay the
placeholder (the trap docs/native-vk.md names).

python -I -B tests/test_transient_targets.py
"""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


class TransientTargetTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tex = (ROOT / "gpu" / "vk" / "vk_texture.cpp").read_text(encoding="utf-8")
        cls.hdr = (ROOT / "gpu" / "vk" / "vk_texture.h").read_text(encoding="utf-8")
        cls.draw = (ROOT / "gpu" / "vk" / "vk_draw.cpp").read_text(encoding="utf-8")

    # ---- (a) per-texture invalidation ----
    def test_a_texture_is_alive_from_its_creation_to_its_destruction(self):
        self.assertIn("alive_set(p->id, true);", block(self.tex, "Texture* create(const Key& key"))
        self.assertIn("alive_set(tex.id, false);", block(self.tex, "void destroy_later(Texture& tex)"))
        alive = block(self.tex, "bool texture_alive(uint32_t id)")
        self.assertIn("memory_order_acquire", alive)   # (read without the lock by the recording threads)

    def test_a_hit_stands_while_its_texture_lives_and_a_miss_until_a_frame_defers(self):
        stands = block(self.draw, "bool handle_hit_stands(")
        self.assertIn("if (!serial) return h.destroy_gen == dgen;", stands)
        self.assertIn("return h.tex ? texture_alive(h.tex_id) : h.destroy_gen == dgen;", stands)
        view = block(self.draw, "VkImageView texture_view_of(")
        self.assertIn("serial ? texture_miss_generation() : texture_destroy_generation()", view)
        self.assertIn("handle_hit_stands(it->second, dgen, serial)", view)
        self.assertEqual(view.count("handle_hits().find"), 1)

    def test_the_frame_that_defers_uploads_moves_the_miss_generation(self):
        frame = block(self.tex, "void texture_frame(uint64_t completed)")
        defer = frame[frame.index("st_deferred_frame.exchange(0"):]
        self.assertIn("destroy_gen++, miss_gen++;", defer[:defer.index("completed_value = completed;")])

    def test_a_kept_descriptor_set_needs_every_texture_it_named_alive(self):
        alive = block(self.draw, "bool desc_textures_alive()")
        self.assertIn("if (!texture_alive(id)) return false;", alive)
        self.assertIn("(!serial || dgen_off || desc_textures_alive())", self.draw)
        self.assertIn("desc_ids().clear(), desc_ids_on = serial;", self.draw)
        self.assertIn("if (serial) last_desc_ids().swap(desc_ids());", self.draw)
        self.assertIn('getenv("RX_VK_TEX_SERIAL")', block(self.tex, "bool texture_serial_on()"))

    # ---- (b) a moved target takes over the image it is made over ----
    def test_only_a_target_unused_this_frame_and_recording_is_taken_over(self):
        take = block(self.tex, "bool take_over(uint64_t key, uint64_t lo, uint64_t size, Texture& x)")
        self.assertIn("c->last_frame >= frame_no || c->last_value == rec_value", take)
        self.assertIn("target_overlap::shifted_over(clo, chi, c->last_frame, lo, hi, frame_no)", take)
        self.assertIn("alive_set(o->id, false);", take)
        self.assertIn("destroy_gen++;", take)
        self.assertIn("forget_gpu(o);", take)
        self.assertIn("taken_over.push_back({frame_no, std::move(it->second)});", take)   # (freed later, not now)

    def test_it_comes_before_the_pool_and_orders_after_the_old_writes(self):
        make = block(self.tex, "Texture* create(const Key& key")
        self.assertLess(make.index("take_over(pool_key, t->gpu, t->L.size, x)"),
                        make.index("pool_take(pool_key, x.image, x.allocation, x.views)"))
        self.assertIn("!taken && !(pool_key && pool_take(", make)
        self.assertIn("if (taken) b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;", make)
        frame = block(self.tex, "void texture_frame(uint64_t completed)")
        self.assertIn("taken_over.front().first + 2 <= frame_no", frame)
        self.assertIn('getenv("RX_VK_TARGET_TAKEOVER")', block(self.tex, "bool takeover_on()"))

    # ---- (c) the creation clear left to a pass ----
    def test_a_pending_clear_is_a_load_op_only_where_the_pass_covers_the_target(self):
        covers = block(self.draw, "bool pass_covers(const Targets& at, const Texture* t)")
        self.assertIn("t->levels == 1 && t->layers == 1 && at.width >= t->width && at.height >= t->height", covers)
        begin = block(self.draw, "void begin_pass(VkCommandBuffer cmd, const Targets& at)")
        self.assertIn("ca[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR", begin)
        self.assertIn("da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR", begin)
        self.assertLess(begin.index("ca[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR"), begin.index('texture_written(at.color[i]'))

    def test_any_other_first_use_clears_it_first(self):
        self.assertIn("if (tex->clear_pending) texture_clear_now(tex);", block(self.tex, "Texture* texture_touch(Texture* tex"))
        self.assertIn("if (tex->clear_pending) texture_clear_now(tex);", block(self.tex, "void texture_written(Texture* tex"))
        self.assertIn("if (tex->clear_pending) return false;", block(self.tex, "bool texture_touch_quick(Texture* tex"))
        make = block(self.tex, "Texture* create(const Key& key")
        self.assertIn('getenv("RX_VK_LAZY_CLEAR")', make)
        self.assertIn("x.levels == 1 && x.layers == 1", make)

    # ---- (d) the GPU's unswizzle for every format and size ----
    def test_bc_and_small_textures_go_to_the_gpu_when_asked_for(self):
        un = block(self.tex, "bool upload_unswizzle_gpu(Texture& tex, uint64_t total, const Ranges* dirty)")
        self.assertIn("(bc && cpu_small)", un)
        self.assertIn("if (cpu_small && total < (8u << 20)) return false;", un)
        self.assertIn("tex.bpb != 8 && tex.bpb != 16", un)
        self.assertIn("const uint32_t per_view = bc ? 1 : tex.layers;", un)   # (block-texel views: a layer each)
        self.assertIn("guest_bytes(tex.gpu, total, scratch)", un)   # (staged, not a pool import)
        # (off by default: no faster than the CPU's on this PC, the copy into staging or the pool imports cost as much)
        self.assertIn('!getenv("RX_VK_CPU_UNSWIZZLE") || atoi(getenv("RX_VK_CPU_UNSWIZZLE")) != 0',
                      block(self.tex, "bool cpu_unswizzle_small()"))


if __name__ == "__main__":
    unittest.main()
