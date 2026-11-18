# Player builder notes

This page is the starting point for a new player builder: a front end that turns a player's dump into a playable game
folder. There is no builder in the tree. Read this before you write one, so it drives the existing steps and keeps the
facts earlier builders learned.

## Run the existing steps

A builder is a thin front end over the developer path ([Development guide](development.md)):

1. **Read the dump.** `hookshot-builder dump <file.nup|folder> --extract <dir>` writes the newest title's `exefs/`
   and `romfs/`, makes ZBIC-compressed modules plain, and replaces damaged `.zs` files (below); `--verify` checks a
   package's hashes without writing. `hookshot-builder build --dump` reads the exefs in memory and copies the romfs
   into the game folder. The lift
   refuses an `exefs/main` other than the supported one.
2. **Repair damaged files.** An incomplete dump (about 23.3 GB instead of 23.8 GB) has damaged `.zs` textures, and the
   game then leaves out walls and floors. The reader replaces each in the `romfs/` it writes with a whole file of the
   same kind and prints what it did; tell the player. A player's own folder is never written to.
3. **Lift and build.** `hookshot-builder build <folder> --direct <hookshot_direct.so>` lifts, compiles and links
   in one process ([the plan](builder-plan.md)). It needs the FFmpeg and glibc development files.
4. **Prepare assets.** `hookshot-builder build ... --dump` writes the game folder's `prepared/tex` (or
   `hookshot-builder textures` alone), the direct back end's texture store ([the converters](direct/converters.md)). Without it every asset texture is a
   placeholder.
5. **Make button prompts.** `tools/prompts/prompt_assets.py --romfs <romfs> --out <game>/prompts` (needs the packages in
   `tools/prompts/prompt_requirements.txt`). Without them the game keeps the Switch's icons.
6. **Assemble the game folder:** `hookshot-builder build --dump` makes it (the executable, `hookshot_direct.so`,
   `image.bin`, `romfs/`, `prepared/` and `user/` for the saves, [the plan](builder-plan.md)); add the SDL library
   and the prompts.

## Report progress

- `hookshot-builder build --progress` prints `@@plan`, `@@file` and `@@eta` lines, estimated from the seconds each
  module took in the last build.
- Name a failed compile by its first `error:` line, not by the last lines of the log: the error is often hundreds of
  lines earlier.

## Facts not to rediscover

- **Memory, not cores, sets the compile's parallelism.** The build starts a compiler only when free memory holds its
  estimate. 8 GB of RAM with no swap closed the game before the title; the game warns about it at start.
- **Free space.** A build needs about 60 GB free on the disk of the game folder. Measure the disk the folder is on.
- **Object cache.** Objects are cached by content in `work/builder-cache`, so an update compiles only what changed.
- **Interrupted steps.** A second build in the same folder must stop the first one first.
- **Read-only installs.** Make files copied from a read-only tools folder writable.
- **Paths are UTF-8** everywhere.
- **Links.** Refuse symbolic links inside the dump's folders, and keep the game's file system inside its mounts
  (`tests/test_fs_links.py`).
