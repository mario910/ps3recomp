# Buzz! Quiz World SDK fork

This is the SDK used by
[buzz-quizworld-recomp](https://github.com/mario910/buzz-quizworld-recomp).
Use the exact commit pinned by that project.

## History and compatibility baseline

The fork's `master` preserves upstream history and the original Buzz development
commits. The runtime and generators deliberately remain on the validated
v0.17.0 baseline `a6d76c895f77eb30b90a806c830544e61821b77f`, plus the Buzz changes.

The fork already had newer v0.18.0 ancestry when this history was restored.
Commit `f9554d9b` explicitly restores the tested baseline; the following merge
brings back our original commits and authorship. This is a fast-forward update
of the existing fork, not a force push or an orphan snapshot.
The newer ancestry is retained, but its runtime/code-generator changes are NOT
claimed to be integrated. A future SDK upgrade must explicitly reverse/revisit
that compatibility pin and validate the resulting runtime and generated code.

The old `buzz-quizworld` source-snapshot branch remains available as a backup.
It is no longer the branch used for new SDK work.

## Publication and licenses

No newly added game/firmware binaries, generated game translations, keys or
private captures are published. Inherited upstream history still contains old
firmware/reference artifacts; restoring history does not purge them or establish
legal clearance. Copied references and old diagnostic logs are omitted from the
current tree, without rewriting historical commits.

Upstream authorship, MIT notices and per-file licenses are retained. In particular,
`third_party/at3_standalone` includes LGPL-2.1-or-later components; consult its
README, COPYING and per-file headers. Generated code from a user's game or
firmware is not relicensed by the SDK's MIT notice.
