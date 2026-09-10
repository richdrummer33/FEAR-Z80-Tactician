# tactical_ai — GOAP/HTN office-loop sim (separate project)

These files are the FEAR-style tactical-AI prototype (`sim.c`, `brain.c`,
`tiles.c`, `main_gg.c`) and are unrelated to the Polar span-interpreter
renderer that the rest of `src/` implements. They were sharing `src/` and were
easy to mistake for renderer sources, so they were moved here.

Build targets are unchanged: `make host`, `make test`, `make gg`,
`make gg-seed42`. `host/main_host.c` and `tests/test_sim.c` belong to this
project too and include these headers by relative path.
