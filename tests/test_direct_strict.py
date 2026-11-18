"""Strict mode and the validation messenger of the direct back end (renderer/direct_check.{h,cpp}), read from the source.

The known list is one table, each entry an event reason the back end really raises (or a validation message id) with a reason
why it is known; strict ends the run with a nonzero code on any other event once armed, and on any validation warning or error;
the switches are HOS_DBG only; every place that counts a skipped or dropped draw, dispatch, clear, copy, report or job also
raises the event, so a skip cannot be counted without strict seeing it.
python -I -B tests/test_direct_strict.py
"""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
D = ROOT / "renderer"


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def c_strings(text):
    return re.findall(r'"((?:[^"\\]|\\.)*)"', text)


class StrictTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.check = (D / "vk_check.cpp").read_text(encoding="utf-8")
        cls.header = (D / "vk_check.h").read_text(encoding="utf-8")
        cls.sources = {p.name: p.read_text(encoding="utf-8") for p in D.glob("*.cpp")}

    def known(self):
        table = self.check[self.check.index("const StrictKnown kStrictKnown[] = {"):]
        table = table[:table.index("};")]
        entries = re.findall(r'\{\s*"((?:[^"\\]|\\.)*)"\s*,\s*((?:"(?:[^"\\]|\\.)*"\s*)+)\}', table)
        return [(r, "".join(c_strings(w))) for r, w in entries], table

    def test_known_list_shape(self):
        entries, table = self.known()
        self.assertTrue(table.rstrip().endswith("{nullptr, nullptr},"), "the table ends with its sentinel")
        self.assertIn("sizeof kStrictKnown / sizeof kStrictKnown[0] - 1", self.check)
        raised = set()
        for text in self.sources.values():
            raised.update(c_strings(text))
        for reason, why in entries:
            self.assertGreater(len(why), 20, "a known entry says why: " + reason)
            if reason.startswith("validation:"):
                self.assertRegex(reason, r"^validation:\S+$")
                continue
            # (a reason the back end raises: a strict_event literal, a name of a counter table the events take, or an object
            # made in play for what an ObjectsFor scope names)
            prefix = "Vulkan object made in steady play: "
            if reason.startswith(prefix):
                fors = set()
                for text in self.sources.values():
                    for m in re.finditer(r"ObjectsFor objects_for\(([^;]*)\);", text):
                        fors.update(c_strings(m.group(1)))
                self.assertIn(reason[len(prefix):], fors, "a known object cause no ObjectsFor names: " + reason)
                continue
            self.assertIn(reason, raised, "a known reason the back end never raises: " + reason)
        self.assertEqual(len({r for r, _ in entries}), len(entries), "each reason once")

    def test_stops_and_logs(self):
        ev = block(self.check, "void strict_event(const char* reason, const char* where, ...) {")
        self.assertIn("if (!strict_enabled) return;", ev)
        self.assertLess(ev.index("if (k) {"), ev.index("stop(86)"))   # (a known reason never stops)
        self.assertIn("strict: STOP at present", ev)
        msg = block(self.check, "VKAPI_ATTR VkBool32 VKAPI_CALL on_message(")
        self.assertIn("stop(87)", msg)
        self.assertIn("s >= 2", msg)   # (warnings and errors)
        stop = block(self.check, "[[noreturn]] void stop(int code) {")
        self.assertLess(stop.index("log_flush()"), stop.index("_exit(code)"))   # (the reason reaches the log)
        made = block(self.check, "void validation_instance_made(VkInstance instance) {")
        self.assertIn("layer_enabled()", made)                                # (asked here or by the loader's variables)
        self.assertIn("strict: known: %s: %s", made)                          # (the known list in the log)
        self.assertIn("kStrictQuiet", self.header)
        arm = block(self.check, "void strict_present(uint64_t present) {")
        self.assertIn("m + kStrictQuiet", arm)
        self.assertIn("strict_present(n)", self.sources["vk_present.cpp"])

    def test_switches(self):
        for name, text in self.sources.items():
            if "getenv" in text:
                self.assertEqual(name, "vk_main.cpp", name + " reads the environment")
                for m in re.finditer(r'getenv\("([^"]+)"\)', text):
                    self.assertEqual(m.group(1), "HOS_DBG")
        self.assertIn('dbg("strict")', self.check)

    def test_every_skip_is_an_event(self):
        draw = self.sources["vk_draw.cpp"]
        count = block(draw, "void count(List* l, Reason r) {")
        self.assertIn("if (r != R_DRAWN && strict_on()) strict_draw(l, r);", count)
        sd = block(draw, "void strict_draw(List* l, Reason r) {")
        self.assertIn("strict_program_miss(", sd)
        self.assertIn("strict_event(kReasons[r]", sd)
        # (every draw skip returns through count(): no other early return in draw_emit records a skip)
        emit = block(draw, "void draw_emit(List* l, uint32_t op, const void* data, uint32_t size, bool recording) {")
        for m in re.finditer(r"return ([^;]*);", emit):
            body = m.group(1)
            self.assertTrue(body == "" or body.startswith("count(l, "), "draw_emit returns without counting: " + body)
        comp = self.sources["vk_compute.cpp"]
        cc = block(comp, "void count(List* l, Reason r) {")
        self.assertIn("strict_dispatch(l, r)", cc)
        passes = self.sources["vk_pass.cpp"]
        pc = block(passes, "void count(int k) {")
        for k in ("S_NO_IMAGE", "S_MASKED", "S_CLEAR_NO_TARGET", "S_CLEAR_DISABLED", "S_COPY_SKIPPED", "S_CLEAR_TEX_SKIPPED", "S_UNRECORDED",
                  "S_JOB_OUTSIDE"):
            self.assertIn(k, pc)
        jc = block(passes, "void jcount(int k) {")
        for k in ("J_NO_FORMATS", "J_MISMATCH", "J_UNENDED", "J_NOT_SECONDARY", "J_NO_PASS"):
            self.assertIn(k, jc)
        self.assertIn("strict_event(kJobNames[J_MISSED]", block(passes, 'extern "C" __attribute__((visibility("default"))) int hsd_pass_known('))
        q = self.sources["vk_query.cpp"]
        qc = block(q, "void count(int k, int64_t n = 1) {")
        for k in ("Q_NO_ROOM", "Q_NOT_MEMORY", "Q_UNRECORDED", "Q_TOO_MANY"):
            self.assertIn(k, qc)
        # (Vulkan objects: one counter, made only through object_made)
        for name, text in self.sources.items():
            if name != "vk_check.cpp":
                self.assertNotIn("st_objects.fetch_add", text, name)
        self.assertIn("const DebugObjects debug_aid;", self.sources["vk_dump.cpp"])
        t = self.sources["vk_task.cpp"]
        self.assertIn("strict_event(kNames[T_REFUSED]", t)


if __name__ == "__main__":
    raise SystemExit(unittest.main(verbosity=1))
