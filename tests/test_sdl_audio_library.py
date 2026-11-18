"""The sound, the window and the controllers load one SDL, the one beside the executable before the system's."""
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SDL = ROOT / "runtime/media/hos_sdl.c"
AUDIO = ROOT / "runtime/nn_audio.c"


class SdlAudioLibraryTests(unittest.TestCase):
    def test_the_bundled_sdl_first(self):
        text = SDL.read_text(encoding="utf-8")
        start = text.index("static os_lib *load_one")
        fn = text[start:text.index("\n}", start)]
        self.assertLess(fn.index("os_exe_path"), fn.index("os_lib_open(name)"))

    def test_sdl3_before_sdl2(self):
        text = SDL.read_text(encoding="utf-8")
        start = text.index("static void load_run(void *arg)")
        fn = text[start:text.index("\n}", start)]
        self.assertLess(fn.index('load_one("libSDL3.so.0")'), fn.index('load_one("libSDL2-2.0.so.0")'))

    def test_audio_takes_the_same_sdl(self):
        text = AUDIO.read_text(encoding="utf-8")
        start = text.index("static int output_try(")
        fn = text[start:text.index("\n}", start)]
        self.assertIn("hos_sdl_audio_open(", fn)
        self.assertNotIn("os_lib_open", fn)

    def test_audio_device_buffer_matches_audio_frame_quantum(self):
        text = SDL.read_text(encoding="utf-8")
        start = text.index("int hos_sdl_audio_open(")
        fn = text[start:text.index("\n}", start)]
        self.assertIn('const char *env_samples = getenv("SDL_AUDIO_DEVICE_SAMPLE_FRAMES");', fn)
        self.assertIn('if (version == 3 && !env_samples) hos_sdl_hint("SDL_AUDIO_DEVICE_SAMPLE_FRAMES", "480");', fn)
        self.assertIn('want.samples = (uint16_t)(env_samples && atoi(env_samples) > 0 ? atoi(env_samples) : 512);', fn)


if __name__ == "__main__":
    raise SystemExit(unittest.main(verbosity=1))
