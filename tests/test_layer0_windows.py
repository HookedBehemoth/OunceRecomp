"""(native-vk, layer 0) The window scheme's arithmetic and the hooks that route guest memory through it.

Builds tests/layer0_windows_test.cpp against gpu/vk/layer0_windows.h (bindings whole in the window of their start,
aliased chunk places, chunk sizes, the bound-chunk bits, first-use spans), and checks in the source that with
RX_VK_LAYER0=1 the buffer lookups, the page table, the host's discard check and the mirrors all go through layer 0
(gpu/vk/vk_pools.cpp), and that the switch is read once.
python -I -B tests/test_layer0_windows.py
"""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


class Layer0WindowTests(unittest.TestCase):
    def test_arithmetic(self):
        compiler = shutil.which("g++") or shutil.which("clang++")
        if not compiler:
            self.skipTest("no C++ compiler")
        with tempfile.TemporaryDirectory(prefix="hookshot-layer0-") as tmp:
            binary = Path(tmp) / "layer0_windows_test"
            build = subprocess.run(
                [compiler, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-pthread", "-I", str(ROOT / "gpu" / "vk"),
                 str(ROOT / "tests" / "layer0_windows_test.cpp"), "-o", str(binary)],
                cwd=ROOT, text=True, capture_output=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(binary)], cwd=ROOT, text=True, capture_output=True, timeout=120)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)

    def test_hooks(self):
        memory = (ROOT / "gpu" / "vk" / "vk_memory.cpp").read_text(encoding="utf-8")
        pools = (ROOT / "gpu" / "vk" / "vk_pools.cpp").read_text(encoding="utf-8")
        bind = block(memory, "BufferRef gpu_buffer_bind(uint64_t gpu, uint64_t size, bool written) {")
        at = bind.index("if (layer0_on())")
        # the virtual pools' translation first, then layer 0, before the lookup's memo, locks and imports
        self.assertLess(bind.index("guest = host->gpu_guest(gpu);"), at)
        for later in ("bind_memo_hit(", "ParLock", "pool_of("):
            self.assertLess(at, bind.index(later), later)
        tail = bind[at:bind.index("return r;", at)]
        self.assertIn("wb_pages_note(guest, guest + size, mem_rec_value)", tail)   # (a written range: the readbacks wait for its GPU work)
        buf = block(memory, "BufferRef gpu_buffer(uint64_t gpu, uint64_t size) {")
        self.assertLess(buf.index("layer0_buffer("), buf.index("pool_of("))
        self.assertIn("layer0_bound(", block(memory, "int memory_guest_imported("))
        self.assertIn("layer0_fill_table()", block(memory, "uint64_t memory_guest_pages() {"))
        self.assertIn("layer0_on() ? 0", block(memory, "bool mirror_on() {"))
        created = block(memory, "void memory_pool_created(uint64_t gpu, uint64_t size) {")
        self.assertLess(created.index("layer0_pool_created("), created.index("bg_start()"))
        # the switch is read once, and the hot path never asks the environment
        on = block(pools, "bool layer0_on() {")
        self.assertIn('static const int want', on)
        for hot in ("BufferRef layer0_bind(", "BufferRef layer0_buffer(", "bool ready_for(", "bool layer0_bound(", "void layer0_cpu_read("):
            self.assertNotIn("getenv", block(pools, hot), hot)
        # chunks are bound before their bits are set (a reader without the lock sees a bit only for a bound chunk)
        b = block(pools, "void bind_range_locked(")
        self.assertLess(b.index("submit_binds(binds)"), b.index("bits->set(c)"))
        self.assertIn("vkWaitForFences", block(pools, "VkResult submit_binds("))
        self.assertLess(b.index("table_set("), b.index("bits->set(c)"))

    def test_vram_class(self):
        """(7b) API 14 carries pool flags and creation keys; placement and give-back keep their order"""
        api = (ROOT / "gpu" / "vk" / "vk_api.h").read_text(encoding="utf-8")
        replay = (ROOT / "gpu" / "vk" / "vk_replay.cpp").read_text(encoding="utf-8")
        native = (ROOT / "host" / "nvn_native.c").read_text(encoding="utf-8")
        pools = (ROOT / "gpu" / "vk" / "vk_pools.cpp").read_text(encoding="utf-8")
        self.assertIn("#define MKWVK_API_VERSION 14", api)
        self.assertIn("void (*nvn_pool_native)(uint64_t gpu, uint64_t size, uint32_t flags, uint64_t key, int on);", api)
        self.assertIn("t.nvn_pool_native = layer0_pool_native;", replay)
        init = block(native, "NV(nvnMemoryPoolInitialize) {")
        self.assertLess(init.index("rx_nvn_pool_native("), init.index("rx_nvn_pool_created("))   # (placed before any eager binding)
        self.assertIn("pool_creation_key(c)", init)
        self.assertIn("rx_nvn_pool_native(o->gpu, o->size, o->flags, 0, 0)", block(native, "NV(nvnMemoryPoolFinalize) {"))
        key = block(native, "static uint64_t pool_creation_key(")
        for guard in ("fp + 16 <= top", "!(fp & 7)", "next <= fp"):
            self.assertIn(guard, key)
        cls = block(pools, "bool vram_class(")
        for need in ("kCpuNoAccess", "kGpuNoAccess", "kPhysical", "kVirtual", "host_keys"):
            self.assertIn(need, cls)
        made = block(pools, "void vram_pool_made(")
        self.assertLess(made.index("submit_binds(binds)"), made.index("ready->set(q)"))   # (bound before the bindings see it)
        rel = block(pools, "void vram_release(")
        self.assertLess(rel.index("wait_for(value)"), rel.index("submit_binds(binds)"))  # (the GPU past its users first)
        self.assertLess(rel.index("ready->clear(q)"), rel.index("submit_binds(binds)"))  # (no new binding of what is unbound)
        native_fn = block(pools, "void layer0_pool_native(")
        self.assertLess(native_fn.index("vram_hand_back("), native_fn.index("wait_for("))  # (a handed-back pool waits for nothing)
        self.assertIn("vram_clear_over(gpu, gpu + size, true)", native_fn)                 # (a CPU-visible pool over VRAM: host)


if __name__ == "__main__":
    raise SystemExit(unittest.main(verbosity=1))
