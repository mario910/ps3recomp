# Buzz! Quiz World SDK source export

This branch is the modified runtime used by
[buzz-quizworld-recomp](https://github.com/mario910/buzz-quizworld-recomp), not
the upstream default SDK. Use the exact commit pinned by that project.
Its README is the authoritative Windows setup/build guide for the game port.

## Provenance

The snapshot is based on Ned Heller / sp00nznet and contributors'
[ps3recomp](https://github.com/sp00nznet/ps3recomp), baseline commit
`a6d76c895f77eb30b90a806c830544e61821b77f` (v0.17.0 integration), with the local
Buzz runtime work through `072a56094cd7eecadbd3c5f1f3e662c6e92f9946`.
MIT copyright notices, CONTRIBUTORS, per-file notices and third-party licenses
are retained. The source export starts a new Git root deliberately; it does
not assert that the upstream work was authored by mario910.

The runtime changes include SPURS job execution/pooling/completion/continuation,
startup environment caches, DMA/coherency handling, D3D12 frame boundaries,
texture/depth/volume/sampler fixes, shader caches, native FP helpers, background
window behavior, audio/runtime-loaded SPU module support and diagnostics. The
game-specific generators, adapters, synthetic regression tests and build
configuration live in the separate port repository.

## Why a new root?

Old upstream ancestry contains Sony firmware PRXs and lifted firmware in
`fw_spu/`, even after upstream removed that directory from the current tree.
This branch does not inherit that ancestry. It also omits copied wiki PDF/text
references and stray diagnostic logs from the snapshot. For method reference,
consult the [PS3 Developer Wiki RSX FIFO page](https://www.psdevwiki.com/ps3/RSXFIFOCommands)
under that site's own terms instead of treating its text as MIT source.

The GitHub repository is still a fork at its owner's request. **Its other
branches/history have not been purged.** Do not confuse this clean branch with
a guarantee about the entire fork network. No firmware/game binaries or
translations from them are included in this branch. Original synthetic
instruction/lifter fixtures are retained for tests.

## Licenses

The toolkit is MIT (`LICENSE`), **except separately licensed components**.
In particular, `third_party/at3_standalone` contains FFmpeg decoders extracted
via PPSSPP, under LGPL-2.1-or-later (see its `README.txt`, per-file headers and
`COPYING.LGPLv2.1`). stb headers carry their embedded MIT/public-domain terms.
Retain those notices; do not describe all bundled code as MIT or distribute
statically linked binaries without checking the applicable LGPL obligations.

Generated game and firmware translations are produced only from the user's
local files, are excluded from publication, and are not relicensed by the MIT
notice. This is a source export, not a ready-to-play game or legal clearance.
