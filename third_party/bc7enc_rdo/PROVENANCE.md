# bc7enc_rdo

The BC encoders `ounce/tex` builds into the builder: `rgbcx` (BC4 and BC5), `bc7decomp` and `bc7e.ispc` (BC7), unchanged
from [richgel999/bc7enc_rdo](https://github.com/richgel999/bc7enc_rdo) at commit
`b9438627eef73a1157e84201b6fa6eb2ffd6d9f0`. Only the files the build reads are kept. The ISPC compiler that builds
`bc7e.ispc` is a tool, not vendored: `HOOKSHOT_ISPC`, else `work/deps/ispc/*/bin/ispc`, else `ispc` on the `PATH`.

License: see `LICENSE` (MIT or the Unlicense; `bc7e.ispc` Apache 2.0).
