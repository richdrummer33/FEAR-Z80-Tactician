/*
 * Ground-truth dirty-region diagnostic.
 *
 * The normal renderer/patch writer may mark g_polar_nt_row_min/max while it
 * works. Ignore those marks. Instead compare all 360 authoritative WRAM name
 * words against a private shadow of the last uploaded state, derive exact row
 * extents from the comparison, then use the mature row uploader.
 *
 * This costs CPU but keeps VDP traffic proportional to ACTUAL changed words,
 * avoiding the VBlank-overrun confound of a literal 720-byte full upload.
 */
#include <stdint.h>
#include "tilesector_polar.h"

extern uint16_t g_map[TSP_MAP_CELLS];
extern uint8_t g_polar_nt_row_min[TSP_ROWS];
extern uint8_t g_polar_nt_row_max[TSP_ROWS];

static uint16_t g_oracle_vram_shadow[TSP_MAP_CELLS];

void tsp_polar_nt_upload_dirty(void);

void tsp_polar_nt_oracle_sync_from_map(void) {
    uint16_t i;
    for(i=0u;i<TSP_MAP_CELLS;++i)g_oracle_vram_shadow[i]=g_map[i];
}

void tsp_polar_nt_upload_oracle(void) {
    uint8_t row;
    uint16_t idx=0u;

    /* Erase any dirty hints produced upstream. This diagnostic accepts only
     * differences independently rediscovered from the full authoritative map. */
    for(row=0u;row<TSP_ROWS;++row){
        uint8_t col;
        uint8_t first=0xffu,last=0u;

        for(col=0u;col<TSP_COLS;++col,++idx){
            uint16_t now=g_map[idx];
            if(now!=g_oracle_vram_shadow[idx]){
                if(first==0xffu)first=col;
                last=col;
                g_oracle_vram_shadow[idx]=now;
            }
        }
        g_polar_nt_row_min[row]=first;
        g_polar_nt_row_max[row]=last;
    }

    tsp_polar_nt_upload_dirty();
}
