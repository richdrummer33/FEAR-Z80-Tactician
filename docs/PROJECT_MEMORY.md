# Project memory / durable conventions

Current checkpoint: **v0.5.0 — Stage 5, banked persistent individual GOAP**.

Durable decisions for future work:

1. Keep authored world data compact and simulation-centric. Static wall occupancy is bit-packed; doors and other sparse dynamic features live in separate compact tables/state.
2. Preserve room-graph long-range navigation plus cheap local steering/BFS fallback. Do not regress to full-map BFS for every actor every tick.
3. Keep individual GOAP persistent: reuse valid plans across ticks and replan only on invalidation or goal change.
4. Keep squad HTN out until the individual brain/combat layer is stable and profiled.
5. Use GBDK autobanking / Sega mapper rather than fighting the 16 KiB fixed bank. Bank high-level AI by subsystem and minimize mapper transitions (currently one brain-bank entry per actor round).
6. Treat the SD flashcart as ROM storage, not extra Game Gear RAM. The challenge target remains a conventional single cartridge image where practical.
7. Curated Game Gear ROM builds are part of the development record and stay committed under `roms/` with versioned filenames. Do not globally ignore `*.gg`.
8. Versioning follows `vMAJOR.MINOR.PATCH`; pre-1.0 capability stages map to the minor number. Current Stage 5 is `v0.5.0`; patch releases are bug/perf/build-only changes.
9. Deterministic reference seeds: seed 2 exercises max 4v7 / eleven actors; seed 42 is the secondary cross-check.
10. Current next direction: enrich autonomous individual combat/GOAP semantics while retaining the compact room-graph foundation; task bindings and shallow squad HTN come afterward.
