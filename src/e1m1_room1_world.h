#ifndef E1M1_ROOM1_WORLD_H
#define E1M1_ROOM1_WORLD_H

#include <stdint.h>

#define E1_INPUT_UP            0x01u
#define E1_INPUT_DOWN          0x02u
#define E1_INPUT_LEFT          0x04u
#define E1_INPUT_RIGHT         0x08u
#define E1_INPUT_SPEED         0x10u
#define E1_INPUT_STRAFE_LEFT   0x20u
#define E1_INPUT_STRAFE_RIGHT  0x40u

#define E1_EYE_HEIGHT_Q4 (5<<4)

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

#endif
