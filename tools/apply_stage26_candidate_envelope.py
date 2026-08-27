#!/usr/bin/env python3
from pathlib import Path

p=Path('src/tilesector_core.c')
s=p.read_text()

if 'STAGE26_CANDIDATE_ENVELOPE' in s:
    print('Stage 26 candidate envelope core hook already applied')
    raise SystemExit(0)

# Stage 12 introduces the GG candidate declaration; retain all later transforms.
anchor='void ts_candidate_span_lite_fast(void);\n'
if anchor not in s:
    raise SystemExit('Stage12 candidate declaration not found')
s=s.replace(anchor,anchor+'void ts_candidate_fast_reset(void);\n',1)

# Charge the one tiny per-frame reset to candidate/visibility itself so the
# candidate-build T bucket measures the complete Stage-26 representation cost.
anchor='''    g_ts_render_stage=3u;
    sector=current_sector(s);
'''
repl='''    g_ts_render_stage=3u;
#ifdef __SDCC
    ts_candidate_fast_reset();
#endif
    sector=current_sector(s);
'''
if anchor not in s:
    raise SystemExit('ts_build_tilemap candidate-stage anchor not found')
s=s.replace(anchor,repl,1)

# Source marker for CI/provenance only.
s=s.replace('void ts_candidate_fast_reset(void);\n',
            'void ts_candidate_fast_reset(void); /* STAGE26_CANDIDATE_ENVELOPE */\n',1)

p.write_text(s)
print('Applied Stage 26 candidate-envelope core reset hook.')
