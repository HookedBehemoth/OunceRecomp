# OunceRecomp documentation

This page lists the documentation pages, grouped by what you want to do. Start with the development guide if you are
new to the code.

## Get started

- [Development guide](development.md): set up, build, run, and the change-and-rebuild loop.
- [Design](design.md): how the parts fit, and the rules every change keeps.

## Test and debug

- [Test OunceRecomp](testing.md): the game-free checks, test runs, the smoke test, benchmarks, recordings and profiles.
- [Switches](switches.md): environment switches, `HOS_DBG` debug aids, and the log format.
- [Reverse engineering](reverse-engineering.md): reading the game's code and files, and labeling functions.

## The lifter

- [The lifter](lifter.md): what it does and how its parts fit.
- [What the lifter emits](lifter-output.md): the code a guest instruction becomes.
- [Write a patch](patches.md): run host code at a guest instruction or replace a function.
- [Build and iterate on the lifter](lifter-build.md): lift or build, caching, inspection and checks.
- [Pessimizations and constraints](lifter-costs.md): known costs and the rules that keep the output correct.
- [Open lifter work](lifter-open-work.md): the queue of output improvements.

## The renderer

- [Renderer docs](direct/README.md): the direct back end and what we know about the game's GPU work.
- [Direct back end plan](direct-backend-plan.md): the rules every renderer change keeps, and its open work.

## Records and guides

- [Parked patches](parked-patches.md): removed features, with what you need to rebuild them.
- [Player builder notes](builder-notes.md): what a new player-facing builder needs.
- [The player builder](builder-plan.md): `ounce-builder`, the dump to a portable game folder in one process.
- [Using a save from an emulator](saves.md): moving saves between OunceRecomp and emulators.
