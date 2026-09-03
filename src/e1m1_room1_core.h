#ifndef E1M1_ROOM1_CORE_H
#define E1M1_ROOM1_CORE_H

#include <stdint.h>

#define E1_COLS 20u
#define E1_ROWS 18u
#define E1_MAP_CELLS (E1_COLS*E1_ROWS)

#define E1_INPUT_UP            0x01u
#define E1_INPUT_DOWN          0x02u
#define E1_INPUT_LEFT          0x04u
#define E1_INPUT_RIGHT         0x08u
#define E1_INPUT_SPEED         0x10u
#define E1_INPUT_STRAFE_LEFT   0x20u
#define E1_INPUT_STRAFE_RIGHT  0x40u

#define E1_SHADE_COUNT 3u
#define E1_BORDER_COUNT 4u
#define E1_CAP_COUNT 3u
#define E1_ATTR_PALETTE 0x0800u
#define E1_TILE_ID_MASK 0x01ffu
#define E1_TILE_CEILING 0u
#define E1_TILE_FLOOR 1u
#define E1_TILE_HORIZON 2u
#define E1_TILE_FULL_BASE 3u
#define E1_CAP_NONE 0u
#define E1_CAP_TOP 1u
#define E1_CAP_BOTTOM 2u
#define E1_TILE_FULL(shade,cap,border) \
    ((uint16_t)(E1_TILE_FULL_BASE + ((((shade)*E1_CAP_COUNT)+(cap))*E1_BORDER_COUNT)+(border)))
#define E1_GENERATED_TILE_COUNT (E1_TILE_FULL_BASE + E1_SHADE_COUNT*E1_CAP_COUNT*E1_BORDER_COUNT)

typedef struct E1Room1State {
    int16_t x_q4;
    int16_t y_q4;
    int16_t z_q4;
    uint8_t yaw;
    int16_t speed_q4;
    int16_t strafe_q4;
    int16_t turn_q4;
    uint8_t speed_scale;
} E1Room1State;

void e1_room1_reset(E1Room1State *s);
void e1_room1_step(E1Room1State *s,uint8_t input);
uint8_t e1_room1_is_walkable_q4(int16_t x_q4,int16_t y_q4);
int16_t e1_room1_floor_z_q4(int16_t x_q4,int16_t y_q4);
void e1_room1_render(const E1Room1State *s,uint16_t out_map[E1_MAP_CELLS]);
uint8_t e1_room1_surface_count(void);

#endif
