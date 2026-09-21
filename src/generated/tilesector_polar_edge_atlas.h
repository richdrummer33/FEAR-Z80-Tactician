/* GENERATED compact polar edge atlas.
 * Static/FULL tile IDs 0..38 remain unchanged.
 * 384 logical no-border EDGE patterns collapse to 80 unique geometries/shade.
 * Shade-1 wall-side LEFT/RIGHT border variants fit while keeping VRAM below
 * the name table at 0x3800.
 */
#ifndef TILESECTOR_POLAR_EDGE_ATLAS_H
#define TILESECTOR_POLAR_EDGE_ATLAS_H
#include <stdint.h>
#define TSP_EDGE_ATLAS_PLAIN_BASE 39u
#define TSP_EDGE_ATLAS_GEOM_COUNT 80u
#define TSP_EDGE_ATLAS_SHADE_STRIDE 80u
#define TSP_EDGE_ATLAS_TILE_COUNT 399u
extern const uint8_t g_tsp_edge_geom_map[128];
#ifdef TSP_EDGE_ATLAS_INIT
static const uint16_t k_tsp_edge_border_phys[160] = {
    16, 279, 280, 281, 282, 283, 284, 285, 286, 287, 288, 289,
    290, 291, 292, 293, 294, 295, 296, 297, 298, 299, 20, 300,
    301, 302, 303, 304, 305, 306, 307, 308, 309, 310, 311, 312,
    313, 314, 315, 316, 317, 318, 319, 320, 321, 322, 323, 324,
    325, 326, 327, 328, 329, 330, 331, 332, 333, 334, 335, 336,
    337, 338, 339, 340, 341, 342, 343, 344, 345, 346, 347, 348,
    349, 350, 351, 114, 115, 116, 117, 0, 17, 17, 352, 353,
    354, 355, 356, 357, 358, 359, 360, 361, 362, 363, 364, 365,
    366, 367, 368, 369, 370, 371, 21, 372, 373, 374, 375, 376,
    377, 148, 378, 379, 380, 381, 382, 383, 155, 156, 384, 385,
    386, 387, 388, 162, 163, 164, 389, 390, 391, 392, 169, 170,
    171, 172, 393, 394, 395, 176, 177, 178, 179, 180, 396, 397,
    183, 184, 185, 186, 187, 398, 189, 190, 191, 192, 193, 114,
    115, 116, 117, 0,
};
#endif
#endif
