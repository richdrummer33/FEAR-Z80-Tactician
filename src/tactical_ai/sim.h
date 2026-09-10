#ifndef GG_CQB_SIM_H
#define GG_CQB_SIM_H

#include <stdint.h>

#define SIM_W 46u
#define SIM_H 24u
#define SIM_CELLS (SIM_W * SIM_H)
#define SIM_BLUE_COUNT 4u
#define SIM_RED_MIN 5u
#define SIM_RED_MAX 7u
#define SIM_MAX_AGENTS (SIM_BLUE_COUNT + SIM_RED_MAX)
#define SIM_MAX_HP 5u
#define SIM_MAX_AMMO 3u
#define SIM_SHOT_RANGE 10u
#define SIM_EVENT_MAX 8u
#define SIM_ROUTE_STEPS 8u
#define SIM_TICK_CAP 1000u
#define SIM_ROOM_COUNT 6u
#define SIM_DOOR_COUNT 7u
#define SIM_PLAN_MAX 4u

/*
 * Stage 5 map RAM is deliberately tiny. Static terrain lives as a 1-bit wall
 * mask in ROM. Only seven door states are dynamic (one bit each in Sim).
 */
typedef enum {
    CELL_FLOOR = 0,
    CELL_WALL,
    CELL_DOOR_CLOSED,
    CELL_DOOR_OPEN
} CellKind;

typedef enum {
    TEAM_BLUE = 0,
    TEAM_RED = 1,
    TEAM_DRAW = 2
} Team;

/* Mirrors the useful individual-goal shape of the desktop FEAR prototype. */
typedef enum {
    GOAL_ESCAPE_DANGER = 0,
    GOAL_COVER,
    GOAL_KILL_ENEMY,
    GOAL_SEARCH_LOST,
    GOAL_PATROL,
    GOAL_IDLE,
    GOAL_COUNT
} GoalKind;

typedef enum {
    ACTION_NONE = 0,
    ACTION_RELOAD,
    ACTION_MOVE_TO_TARGET,
    ACTION_SHOOT,
    ACTION_MOVE_TO_COVER,
    ACTION_MOVE_TO_LAST_SEEN,
    ACTION_MOVE_PATROL,
    ACTION_WAIT,
    ACTION_COUNT
} ActionKind;

/* Compact symbolic GOAP facts. Concrete targets/positions remain bindings. */
enum {
    FACT_TARGET_KNOWN = 1u << 0,
    FACT_IN_SHOT_RANGE = 1u << 1,
    FACT_WEAPON_READY = 1u << 2,
    FACT_TARGET_DEAD = 1u << 3,
    FACT_IN_COVER = 1u << 4,
    FACT_AT_LAST_SEEN = 1u << 5,
    FACT_AT_PATROL = 1u << 6,
    FACT_WAITED = 1u << 7
};

typedef enum {
    EVENT_NONE = 0,
    EVENT_MOVE,
    EVENT_DOOR,
    EVENT_SHOT_MISS,
    EVENT_SHOT_HIT,
    EVENT_KILL,
    EVENT_RELOAD
} EventKind;

typedef struct {
    uint8_t x;
    uint8_t y;
    uint8_t home_x;
    uint8_t home_y;
    uint8_t hp;
    uint8_t ammo;
    uint8_t alive;
    uint8_t suppressed;
    uint8_t hit_flash;
    uint8_t last_seen_x;
    uint8_t last_seen_y;
    uint8_t last_seen_valid;
    uint8_t last_seen_enemy;
    uint16_t last_seen_tick;

    /* Individual goal arbitration + persistent GOAP execution state. */
    uint8_t goal; /* GoalKind */
    uint8_t plan_goal;
    uint8_t plan_target;
    uint8_t plan_tx;
    uint8_t plan_ty;
    uint8_t plan_len;
    uint8_t plan_pos;
    uint8_t plan_age;
    uint8_t plan[SIM_PLAN_MAX]; /* ActionKind */
    uint16_t facts;
    uint16_t replan_count;
    uint16_t reuse_count;

    /* Eight-step path cache: expensive searches amortize over several ticks. */
    uint8_t route_tx;
    uint8_t route_ty;
    uint8_t route_len;
    uint8_t route_pos;
    uint8_t route[SIM_ROUTE_STEPS];
} Agent;

typedef struct {
    uint16_t tick;
    uint16_t rng;
    uint8_t done;
    uint8_t winner; /* Team */
    uint8_t red_count;
    uint8_t agent_count;
    uint8_t door_open_mask;
    Agent agents[SIM_MAX_AGENTS];
    struct {
        uint8_t kind; /* EventKind */
        uint8_t x;
        uint8_t y;
        uint8_t team;
    } events[SIM_EVENT_MAX];
    uint8_t event_count;
    uint16_t last_acted_mask;
    uint16_t total_replans;
    uint16_t total_plan_reuses;
} Sim;

void sim_init(Sim *s, uint16_t seed);
void sim_tick(Sim *s);
uint8_t sim_has_los(const Sim *s, uint8_t ax, uint8_t ay, uint8_t bx, uint8_t by);
uint8_t sim_cell_passable(const Sim *s, uint8_t x, uint8_t y, uint8_t allow_closed_door);
uint8_t sim_cell_kind(const Sim *s, uint8_t x, uint8_t y);
uint8_t sim_agent_team(uint8_t id);
uint8_t sim_team_alive(const Sim *s, uint8_t team);
uint8_t sim_team_hp(const Sim *s, uint8_t team);
const char *sim_goal_name(GoalKind goal);
const char *sim_action_name(ActionKind action);

/* Non-banked primitive API consumed by the banked individual brain. */
uint8_t sim_nearest_visible_enemy(const Sim *s, uint8_t id);
void sim_perceive(Sim *s, uint8_t id, uint8_t visible_enemy);
uint8_t sim_has_adjacent_cover(const Sim *s, uint8_t x, uint8_t y);
uint8_t sim_pick_cover_tile(const Sim *s, uint8_t id, uint8_t threat_id, uint8_t *out_x, uint8_t *out_y);
void sim_do_shot(Sim *s, uint8_t id, uint8_t target_id);
void sim_do_move_or_door(Sim *s, uint8_t id, uint8_t tx, uint8_t ty);
void sim_patrol_target(const Sim *s, uint8_t id, uint8_t *tx, uint8_t *ty);
uint8_t sim_target_visible(const Sim *s, uint8_t id, uint8_t target_id);
uint8_t sim_target_in_range(const Sim *s, uint8_t id, uint8_t target_id);
void sim_record_reload(Sim *s, uint8_t id);

#endif
