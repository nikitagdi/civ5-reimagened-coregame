# Profile-guided build of the Linux server core

`gamecore-linux.profdata` is the clang profile (`llvm-profdata merge --sparse`) of the Linux core that `cargo xtask deploy
--pgo` / `cargo xtask gamecore --pgo` / `build.sh --target linux --pgo` build with. `gamecore-linux.profdata.txt` says what it was
made from (fork commit, compiler, training saves, sha1).

* **What it does.** `-fprofile-instr-use` changes inlining, block order and code layout of the portable build. It does not change
  what the code computes: the digest of the state and the core's part of the save (GCS1) of every turn start are equal with and
  without it (the gate below). It gains about 12 % on the AI phase of a late Huge game. The wasm core (replica in the browser) is
  not profile-guided: it does not run the AI, and `build.sh` refuses `--pgo` for `--target wasm`.
* **Build id.** Unchanged. The id (`<tag>-fx<count>-<hash>`) covers the `LEKMOD_HEADLESS_FIX_*` / `HYG_*` defines only; OPT defines
  and compiler flags are outside it (an OPT or a codegen option changes no result). PGO is a compiler option, so a save written by a
  PGO core loads in a plain core and the other way round, and `cargo xtask deploy` does not see a "core change" because of it.
  `build-info.txt` records `pgo: use <sha1 of the profile>` for traceability.
* **Stale profile.** The profile belongs to a source state. clang ignores the profile of a function whose control flow changed and
  builds it as without PGO, so a stale profile is still correct, only less fast. Refresh it (below) when the fork changes the AI or
  the pathfinding code in a way that matters for speed.
* **Refresh.** `tools/pgo/train.sh` of civ5-reimagened (needs the saves, which are not in git): builds the instrumented core
  (`build.sh --pgo-gen`), plays the training saves with the `live_hang` harness, merges the `.profraw` files into this directory.
  Commit the new `.profdata` and `.txt`. The profile must come from a build that has the same OPT defines as the one it is used for.
* **Gate.** `tools/pgo/compare.sh` of civ5-reimagened builds the plain and the profile-guided core, plays six saves (turns the
  profile was not trained on among them) and compares digest and save hash per turn; it exits 1 on any difference.
