#include "sim.h"
#include "brain.h"

#define NO_AGENT   0xffu
#define COVER_RADIUS 4u

/*
 * Stage 5 retains the original authored FEAR-style office-loop topology.
 * The full world is 46 x 24 logical cells; Game Gear rendering packs each
 * 2 x 2 group into one 8 x 8 background pattern (4 pixels per sim cell).
 */
static const uint8_t k_room_bounds[SIM_ROOM_COUNT][4] = {
    { 1u,  8u,  8u, 15u}, /* Entry */
    {10u,  6u, 18u, 17u}, /* Lobby */
    {20u,  2u, 32u, 12u}, /* Bullpen */
    {20u, 14u, 28u, 21u}, /* Records */
    {34u,  4u, 44u, 12u}, /* Security */
    {30u, 14u, 44u, 21u}  /* Service */
};

/* Persistent patrol anchors create traffic through both halves of the office loop. */
static const uint8_t k_blue_patrol_x[SIM_BLUE_COUNT] = {39u, 37u, 26u, 24u};
static const uint8_t k_blue_patrol_y[SIM_BLUE_COUNT] = { 8u, 18u,  7u, 18u};
static const uint8_t k_red_patrol_x[SIM_RED_MAX] = {4u, 5u, 12u, 16u, 6u, 14u, 3u};
static const uint8_t k_red_patrol_y[SIM_RED_MAX] = {11u,13u,11u,15u,9u, 7u,14u};

/* Coarse navigation metadata for the authored six-room office graph. */
static const uint8_t k_door_rooms[7][2] = {
    {0u,1u}, {1u,2u}, {1u,3u}, {2u,4u}, {3u,5u}, {4u,5u}, {2u,5u}
};
static const uint8_t k_door_x[7] = {9u,19u,19u,33u,29u,38u,31u};
static const uint8_t k_door_y[7] = {11u,8u,16u,8u,17u,13u,13u};
static const uint8_t k_room_dist[SIM_ROOM_COUNT][SIM_ROOM_COUNT] = {
    {0u,1u,2u,2u,3u,3u},
    {1u,0u,1u,1u,2u,2u},
    {2u,1u,0u,2u,1u,1u},
    {2u,1u,2u,0u,2u,1u},
    {3u,2u,1u,2u,0u,1u},
    {3u,2u,1u,1u,1u,0u}
};

/*
 * Shared navigation scratch. Parent directions are 2-bit packed and visited is
 * one bit per map cell. The BFS queue is capped at 256 local cells; long-range
 * travel should normally be handled by the six-room graph before this fallback.
 */
#define NAV_QUEUE_MAX 256u
static uint8_t nav_visited[(SIM_CELLS + 7u) / 8u];
static uint8_t nav_parent2[(SIM_CELLS + 3u) / 4u];
static uint8_t nav_qx[NAV_QUEUE_MAX];
static uint8_t nav_qy[NAV_QUEUE_MAX];

static const int8_t k_dx[4] = { 0, 1, 0, -1 };
static const int8_t k_dy[4] = { -1, 0, 1, 0 };

static uint16_t cell_index(uint8_t x, uint8_t y) {
    return (uint16_t)y * SIM_W + x;
}

static uint8_t in_bounds_i(int16_t x, int16_t y) {
    return (x >= 0 && x < (int16_t)SIM_W && y >= 0 && y < (int16_t)SIM_H) ? 1u : 0u;
}

uint8_t sim_agent_team(uint8_t id) {
    return (id < SIM_BLUE_COUNT) ? TEAM_BLUE : TEAM_RED;
}

static uint8_t local_index(uint8_t id) {
    return (id < SIM_BLUE_COUNT) ? id : (uint8_t)(id - SIM_BLUE_COUNT);
}

uint8_t sim_team_alive(const Sim *s, uint8_t team) {
    uint8_t i, n = 0u;
    uint8_t first = (team == TEAM_BLUE) ? 0u : SIM_BLUE_COUNT;
    uint8_t count = (team == TEAM_BLUE) ? SIM_BLUE_COUNT : s->red_count;
    for (i = 0u; i < count; ++i) if (s->agents[first + i].alive) ++n;
    return n;
}

uint8_t sim_team_hp(const Sim *s, uint8_t team) {
    uint8_t i, hp = 0u;
    uint8_t first = (team == TEAM_BLUE) ? 0u : SIM_BLUE_COUNT;
    uint8_t count = (team == TEAM_BLUE) ? SIM_BLUE_COUNT : s->red_count;
    for (i = 0u; i < count; ++i) hp = (uint8_t)(hp + s->agents[first + i].hp);
    return hp;
}

static uint16_t rng_next(Sim *s) {
    uint16_t x = s->rng;
    if (x == 0u) x = 0xace1u;
    x ^= (uint16_t)(x << 7);
    x ^= (uint16_t)(x >> 9);
    x ^= (uint16_t)(x << 8);
    s->rng = x;
    return x;
}

static uint8_t rng_percent(Sim *s, uint8_t pct) {
    return (uint8_t)(rng_next(s) % 100u) < pct;
}

static uint8_t rng_range_u8(Sim *s, uint8_t lo, uint8_t hi) {
    uint8_t span = (uint8_t)(hi - lo + 1u);
    return (uint8_t)(lo + (uint8_t)(rng_next(s) % span));
}

/*
 * Static office-loop terrain as a one-bit wall mask in ROM: 1104 cells become
 * 138 bytes. Door coordinates are stored separately; only their seven open/
 * closed bits live in RAM. This is the compact representation Rich suggested.
 */
static const uint8_t k_wall_bits[(SIM_CELLS + 7u) / 8u] = {
    0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0x00u, 0xe0u,
    0xffu, 0xffu, 0xffu, 0x3fu, 0x00u, 0xf8u, 0xffu, 0xffu, 0xffu, 0x0fu, 0x40u, 0x02u, 0xe0u, 0xffu, 0xffu, 0x63u,
    0x8bu, 0x00u, 0xf8u, 0x3fu, 0x80u, 0x00u, 0x20u, 0x00u, 0xfeu, 0x0fu, 0x20u, 0x00u, 0x88u, 0x8du, 0x01u, 0x02u,
    0x00u, 0x00u, 0x00u, 0x60u, 0x80u, 0x10u, 0xa2u, 0x8du, 0x00u, 0x18u, 0x20u, 0x80u, 0x00u, 0x24u, 0x00u, 0x06u,
    0x00u, 0x20u, 0x00u, 0x08u, 0x80u, 0x01u, 0x02u, 0x08u, 0x00u, 0x02u, 0x60u, 0x80u, 0x00u, 0xfeu, 0xdfu, 0xefu,
    0x1fu, 0x20u, 0x84u, 0x00u, 0x02u, 0x00u, 0x06u, 0x08u, 0x20u, 0x88u, 0x00u, 0x80u, 0xffu, 0x03u, 0x40u, 0x20u,
    0x00u, 0xe0u, 0xffu, 0x00u, 0x92u, 0x80u, 0x6eu, 0xf8u, 0xffu, 0xffu, 0x20u, 0x02u, 0x00u, 0xfeu, 0xffu, 0x3fu,
    0x81u, 0x00u, 0x90u, 0xffu, 0xffu, 0x4fu, 0x20u, 0x00u, 0xe0u, 0xffu, 0xffu, 0x03u, 0x08u, 0x00u, 0xf8u, 0xffu,
    0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu
};

static const uint8_t k_door_bits[(SIM_CELLS + 7u) / 8u] = {
    0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
    0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
    0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
    0x08u, 0x00u, 0x02u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
    0x08u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x20u, 0x10u,
    0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x08u, 0x00u,
    0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x08u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
    0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
    0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u
};

static uint8_t abs_u8_diff(uint8_t a, uint8_t b) {
    return (a > b) ? (uint8_t)(a - b) : (uint8_t)(b - a);
}

static uint8_t manhattan(uint8_t ax, uint8_t ay, uint8_t bx, uint8_t by) {
    return (uint8_t)(abs_u8_diff(ax, bx) + abs_u8_diff(ay, by));
}

static uint16_t dist2(uint8_t ax, uint8_t ay, uint8_t bx, uint8_t by) {
    int16_t dx = (int16_t)ax - (int16_t)bx;
    int16_t dy = (int16_t)ay - (int16_t)by;
    return (uint16_t)(dx * dx + dy * dy);
}

static int8_t door_at(uint8_t x, uint8_t y);

uint8_t sim_cell_kind(const Sim *s, uint8_t x, uint8_t y) {
    uint16_t i;
    if (x >= SIM_W || y >= SIM_H) return CELL_WALL;
    i = cell_index(x, y);
    if (k_door_bits[i >> 3] & (uint8_t)(1u << (i & 7u))) {
        int8_t d = door_at(x, y);
        if (d >= 0) return (s->door_open_mask & (uint8_t)(1u << (uint8_t)d)) ? CELL_DOOR_OPEN : CELL_DOOR_CLOSED;
    }
    return (k_wall_bits[i >> 3] & (uint8_t)(1u << (i & 7u))) ? CELL_WALL : CELL_FLOOR;
}

static uint8_t cell_blocks_los(const Sim *s, uint8_t x, uint8_t y) {
    uint8_t c = sim_cell_kind(s, x, y);
    return (c == CELL_WALL || c == CELL_DOOR_CLOSED) ? 1u : 0u;
}

uint8_t sim_cell_passable(const Sim *s, uint8_t x, uint8_t y, uint8_t allow_closed_door) {
    uint8_t c;
    if (x >= SIM_W || y >= SIM_H) return 0u;
    c = sim_cell_kind(s, x, y);
    if (c == CELL_FLOOR || c == CELL_DOOR_OPEN) return 1u;
    if (allow_closed_door && c == CELL_DOOR_CLOSED) return 1u;
    return 0u;
}

uint8_t sim_has_los(const Sim *s, uint8_t ax, uint8_t ay, uint8_t bx, uint8_t by) {
    int16_t x0 = ax, y0 = ay, x1 = bx, y1 = by;
    int16_t dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
    int16_t sx = (x0 < x1) ? 1 : -1;
    int16_t dy_abs = (y1 > y0) ? (y1 - y0) : (y0 - y1);
    int16_t dy = -dy_abs;
    int16_t sy = (y0 < y1) ? 1 : -1;
    int16_t err = dx + dy;

    for (;;) {
        int16_t e2;
        if (x0 == x1 && y0 == y1) return 1u;
        e2 = (int16_t)(2 * err);
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
        if (x0 == x1 && y0 == y1) return 1u;
        if (!in_bounds_i(x0, y0)) return 0u;
        if (cell_blocks_los(s, (uint8_t)x0, (uint8_t)y0)) return 0u;
    }
}

static uint8_t occupied_by_other(const Sim *s, uint8_t mover, uint8_t x, uint8_t y) {
    uint8_t i;
    for (i = 0u; i < s->agent_count; ++i) {
        if (i == mover) continue;
        if (s->agents[i].alive && s->agents[i].x == x && s->agents[i].y == y) return 1u;
    }
    return 0u;
}

static uint8_t random_floor_in_room(Sim *s, uint8_t mover, uint8_t room, uint8_t *out_x, uint8_t *out_y) {
    uint8_t tries;
    const uint8_t *r = k_room_bounds[room];
    for (tries = 0u; tries < 96u; ++tries) {
        uint8_t x = rng_range_u8(s, r[0], r[2]);
        uint8_t y = rng_range_u8(s, r[1], r[3]);
        if (!sim_cell_passable(s, x, y, 0u)) continue;
        if (occupied_by_other(s, mover, x, y)) continue;
        *out_x = x; *out_y = y; return 1u;
    }
    return 0u;
}

static void init_agent(Sim *s, uint8_t id, uint8_t room) {
    Agent *a = &s->agents[id];
    uint8_t j, x = k_room_bounds[room][0], y = k_room_bounds[room][1];
    (void)random_floor_in_room(s, id, room, &x, &y);
    a->x = x; a->y = y;
    a->home_x = x; a->home_y = y;
    a->hp = SIM_MAX_HP; a->ammo = SIM_MAX_AMMO; a->alive = 1u;
    a->suppressed = 0u; a->hit_flash = 0u;
    a->last_seen_x = 0u; a->last_seen_y = 0u;
    a->last_seen_valid = 0u; a->last_seen_enemy = NO_AGENT;
    a->last_seen_tick = 0u; a->goal = GOAL_PATROL;
    a->plan_goal = GOAL_IDLE; a->plan_target = NO_AGENT; a->plan_tx = a->plan_ty = 0u;
    a->plan_len = a->plan_pos = a->plan_age = 0u; a->facts = 0u;
    a->replan_count = a->reuse_count = 0u;
    for (j = 0u; j < SIM_PLAN_MAX; ++j) a->plan[j] = ACTION_NONE;
    a->route_tx = 0u; a->route_ty = 0u; a->route_len = 0u; a->route_pos = 0u;
    for (j = 0u; j < SIM_ROUTE_STEPS; ++j) a->route[j] = 0u;
}

uint8_t sim_has_adjacent_cover(const Sim *s, uint8_t x, uint8_t y) {
    uint8_t d;
    for (d = 0u; d < 4u; ++d) {
        int16_t nx = (int16_t)x + k_dx[d];
        int16_t ny = (int16_t)y + k_dy[d];
        if (!in_bounds_i(nx, ny)) continue;
        if (cell_blocks_los(s, (uint8_t)nx, (uint8_t)ny)) return 1u;
    }
    return 0u;
}

static void invalidate_route(Agent *a) {
    a->route_len = 0u;
    a->route_pos = 0u;
}

/*
 * Build and cache only the next eight route steps. That amortizes one BFS over
 * several logical ticks without making each Agent carry a full-map route.
 */
static uint8_t nav_seen(uint16_t i) {
    return (nav_visited[i >> 3] & (uint8_t)(1u << (i & 7u))) ? 1u : 0u;
}

static void nav_mark(uint16_t i, uint8_t parent_dir) {
    uint8_t shift = (uint8_t)((i & 3u) << 1);
    nav_visited[i >> 3] |= (uint8_t)(1u << (i & 7u));
    nav_parent2[i >> 2] = (uint8_t)((nav_parent2[i >> 2] & (uint8_t)~(3u << shift)) |
                                      (uint8_t)((parent_dir & 3u) << shift));
}

static uint8_t nav_parent_dir(uint16_t i) {
    uint8_t shift = (uint8_t)((i & 3u) << 1);
    return (uint8_t)((nav_parent2[i >> 2] >> shift) & 3u);
}

/*
 * Build and cache only the next eight route steps. The fallback BFS is bounded
 * to 256 queued cells because room-graph waypoints handle long travel first.
 */
static uint8_t build_route(Sim *s, uint8_t mover, uint8_t tx, uint8_t ty) {
    Agent *a = &s->agents[mover];
    uint16_t i, head = 0u, tail = 0u;
    uint8_t path_len = 0u;
    uint8_t found = 0u;
    uint8_t wx = 0u, wy = 0u;

    invalidate_route(a);
    a->route_tx = tx;
    a->route_ty = ty;
    if (a->x == tx && a->y == ty) return 0u;

    for (i = 0u; i < (uint16_t)sizeof(nav_visited); ++i) nav_visited[i] = 0u;
    nav_mark(cell_index(a->x, a->y), 0u);
    nav_qx[tail] = a->x;
    nav_qy[tail] = a->y;
    ++tail;

    while (head < tail && !found) {
        uint8_t cx = nav_qx[head];
        uint8_t cy = nav_qy[head];
        uint8_t d;
        ++head;

        for (d = 0u; d < 4u; ++d) {
            int16_t nx_i = (int16_t)cx + k_dx[d];
            int16_t ny_i = (int16_t)cy + k_dy[d];
            uint8_t nx, ny;
            uint16_t ni;
            if (!in_bounds_i(nx_i, ny_i)) continue;
            nx = (uint8_t)nx_i;
            ny = (uint8_t)ny_i;
            ni = cell_index(nx, ny);
            if (nav_seen(ni)) continue;
            if (!sim_cell_passable(s, nx, ny, 1u)) continue;

            nav_mark(ni, (uint8_t)((d + 2u) & 3u)); /* child -> parent */
            if (nx == tx && ny == ty) { wx = nx; wy = ny; found = 1u; break; }
            if (tail < NAV_QUEUE_MAX) {
                nav_qx[tail] = nx;
                nav_qy[tail] = ny;
                ++tail;
            }
        }
    }
    if (!found) return 0u;

    /* Re-use nav_qx as a reverse direction stack after the BFS has finished. */
    while (!(wx == a->x && wy == a->y) && path_len < (NAV_QUEUE_MAX - 1u)) {
        uint8_t pd = nav_parent_dir(cell_index(wx, wy));
        nav_qx[path_len++] = (uint8_t)((pd + 2u) & 3u); /* parent -> child */
        wx = (uint8_t)((int16_t)wx + k_dx[pd]);
        wy = (uint8_t)((int16_t)wy + k_dy[pd]);
    }

    if (!path_len) return 0u;
    a->route_len = path_len < SIM_ROUTE_STEPS ? path_len : SIM_ROUTE_STEPS;
    a->route_pos = 0u;
    for (i = 0u; i < a->route_len; ++i)
        a->route[i] = nav_qx[(uint8_t)(path_len - 1u - (uint8_t)i)];
    return 1u;
}

static uint8_t follow_route(Sim *s, uint8_t id, uint8_t tx, uint8_t ty) {
    Agent *a = &s->agents[id];
    uint8_t d, nx, ny;
    uint16_t ni;

    if (a->route_pos >= a->route_len || a->route_tx != tx || a->route_ty != ty) {
        if (!build_route(s, id, tx, ty)) return 0u;
    }
    d = a->route[a->route_pos];
    nx = (uint8_t)((int16_t)a->x + k_dx[d]);
    ny = (uint8_t)((int16_t)a->y + k_dy[d]);
    if (!sim_cell_passable(s, nx, ny, 1u)) { invalidate_route(a); return 0u; }
    if (occupied_by_other(s, id, nx, ny)) return 0u; /* transient traffic: keep cached route */

    ni = cell_index(nx, ny);
    (void)ni;
    if (sim_cell_kind(s, nx, ny) == CELL_DOOR_CLOSED) {
        int8_t door = door_at(nx, ny);
        if (door >= 0) s->door_open_mask |= (uint8_t)(1u << (uint8_t)door);
        return 2u; /* door opened; keep this route step for next tick */
    }
    a->x = nx;
    a->y = ny;
    ++a->route_pos;
    return 1u;
}

uint8_t sim_nearest_visible_enemy(const Sim *s, uint8_t id) {
    const Agent *a = &s->agents[id];
    uint8_t first = (sim_agent_team(id) == TEAM_BLUE) ? SIM_BLUE_COUNT : 0u;
    uint8_t count = (sim_agent_team(id) == TEAM_BLUE) ? s->red_count : SIM_BLUE_COUNT;
    uint8_t i, best = NO_AGENT;
    uint16_t best_d2 = 0xffffu;
    for (i = 0u; i < count; ++i) {
        uint8_t eid = (uint8_t)(first + i);
        const Agent *e = &s->agents[eid];
        uint16_t d;
        if (!e->alive) continue;
        if (!sim_has_los(s, a->x, a->y, e->x, e->y)) continue;
        d = dist2(a->x, a->y, e->x, e->y);
        if (d < best_d2) { best_d2 = d; best = eid; }
    }
    return best;
}

void sim_perceive(Sim *s, uint8_t id, uint8_t visible_enemy) {
    Agent *a = &s->agents[id];
    if (visible_enemy == NO_AGENT) return;
    a->last_seen_enemy = visible_enemy;
    a->last_seen_x = s->agents[visible_enemy].x;
    a->last_seen_y = s->agents[visible_enemy].y;
    a->last_seen_valid = 1u;
    a->last_seen_tick = s->tick;
}

/* Search only near the actor, not all 1104 cells. Same idea; much kinder to Z80. */
uint8_t sim_pick_cover_tile(const Sim *s, uint8_t id, uint8_t threat_id, uint8_t *out_x, uint8_t *out_y) {
    const Agent *a = &s->agents[id];
    const Agent *e = &s->agents[threat_id];
    uint16_t best_score = 0xffffu;
    uint8_t found = 0u;
    int16_t y, x;
    int16_t min_x = (int16_t)a->x - COVER_RADIUS;
    int16_t max_x = (int16_t)a->x + COVER_RADIUS;
    int16_t min_y = (int16_t)a->y - COVER_RADIUS;
    int16_t max_y = (int16_t)a->y + COVER_RADIUS;

    if (min_x < 1) min_x = 1;
    if (min_y < 1) min_y = 1;
    if (max_x > (int16_t)SIM_W - 2) max_x = (int16_t)SIM_W - 2;
    if (max_y > (int16_t)SIM_H - 2) max_y = (int16_t)SIM_H - 2;

    for (y = min_y; y <= max_y; ++y) {
        for (x = min_x; x <= max_x; ++x) {
            uint16_t score;
            uint8_t exposed;
            uint8_t ux = (uint8_t)x, uy = (uint8_t)y;
            if (!sim_cell_passable(s, ux, uy, 0u)) continue;
            if (occupied_by_other(s, id, ux, uy)) continue;
            if (!sim_has_adjacent_cover(s, ux, uy)) continue;
            exposed = sim_has_los(s, e->x, e->y, ux, uy);
            score = (uint16_t)manhattan(a->x, a->y, ux, uy) * 3u;
            if (exposed) score += 28u;
            if (score < best_score) {
                best_score = score;
                *out_x = ux; *out_y = uy;
                found = 1u;
            }
        }
    }
    return found;
}

static uint8_t target_has_cover(const Sim *s, const Agent *target) {
    return sim_has_adjacent_cover(s, target->x, target->y);
}

static void record_event(Sim *s, uint8_t kind, uint8_t x, uint8_t y, uint8_t team) {
    uint8_t slot = s->event_count;
    if (slot >= SIM_EVENT_MAX) slot = (uint8_t)(SIM_EVENT_MAX - 1u);
    s->events[slot].kind = kind;
    s->events[slot].x = x;
    s->events[slot].y = y;
    s->events[slot].team = team;
    if (s->event_count < SIM_EVENT_MAX) ++s->event_count;
}

static void check_winner(Sim *s) {
    uint8_t ba = sim_team_alive(s, TEAM_BLUE);
    uint8_t ra = sim_team_alive(s, TEAM_RED);
    if (ba && ra) return;
    s->done = 1u;
    if (ba && !ra) s->winner = TEAM_BLUE;
    else if (ra && !ba) s->winner = TEAM_RED;
    else s->winner = TEAM_DRAW;
}

void sim_do_shot(Sim *s, uint8_t id, uint8_t target_id) {
    Agent *a = &s->agents[id];
    Agent *e = &s->agents[target_id];
    uint8_t d = manhattan(a->x, a->y, e->x, e->y);
    int16_t chance = (int16_t)82 - (int16_t)d * 4;
    uint8_t damage;
    if (target_has_cover(s, e)) chance -= 22;
    if (a->suppressed) chance -= 14;
    if (chance < 15) chance = 15;
    if (chance > 82) chance = 82;

    if (!rng_percent(s, (uint8_t)chance)) {
        e->suppressed = 2u;
        record_event(s, EVENT_SHOT_MISS, e->x, e->y, sim_agent_team(id));
        return;
    }

    damage = rng_percent(s, 16u) ? 2u : 1u;
    if (damage >= e->hp) e->hp = 0u;
    else e->hp = (uint8_t)(e->hp - damage);
    e->hit_flash = 1u;
    if (e->hp == 0u) {
        e->alive = 0u;
        invalidate_route(e);
        record_event(s, EVENT_KILL, e->x, e->y, sim_agent_team(id));
        check_winner(s);
    } else {
        e->suppressed = 2u;
        record_event(s, EVENT_SHOT_HIT, e->x, e->y, sim_agent_team(id));
    }
}

static int8_t room_at(uint8_t x, uint8_t y) {
    uint8_t r;
    for (r = 0u; r < SIM_ROOM_COUNT; ++r) {
        const uint8_t *b = k_room_bounds[r];
        if (x >= b[0] && x <= b[2] && y >= b[1] && y <= b[3]) return (int8_t)r;
    }
    return -1;
}

static int8_t door_at(uint8_t x, uint8_t y) {
    uint8_t d;
    for (d = 0u; d < 7u; ++d) if (k_door_x[d] == x && k_door_y[d] == y) return (int8_t)d;
    return -1;
}

/*
 * Pick a cheap room-graph waypoint before touching tile BFS. This is the key
 * scaling trick for the 46x24 authored map: long travel reasons over six rooms
 * and seven connectors; tile search is only a local escape hatch.
 */
static void room_waypoint(const Agent *a, uint8_t tx, uint8_t ty, uint8_t *wx, uint8_t *wy) {
    int8_t cr = room_at(a->x, a->y);
    int8_t tr = room_at(tx, ty);
    int8_t cd = door_at(a->x, a->y);
    uint8_t d;
    *wx = tx; *wy = ty;
    if (tr < 0) return;

    if (cr >= 0 && cr != tr) {
        uint8_t best_d = 0xffu, best_cost = 0xffu;
        for (d = 0u; d < 7u; ++d) {
            uint8_t aroom = k_door_rooms[d][0], broom = k_door_rooms[d][1], nr;
            if (aroom == (uint8_t)cr) nr = broom;
            else if (broom == (uint8_t)cr) nr = aroom;
            else continue;
            if (k_room_dist[nr][(uint8_t)tr] < best_cost) {
                best_cost = k_room_dist[nr][(uint8_t)tr]; best_d = d;
            }
        }
        if (best_d != 0xffu) { *wx = k_door_x[best_d]; *wy = k_door_y[best_d]; }
        return;
    }

    if (cr < 0 && cd >= 0) {
        uint8_t ra = k_door_rooms[(uint8_t)cd][0], rb = k_door_rooms[(uint8_t)cd][1];
        uint8_t nr = (k_room_dist[ra][(uint8_t)tr] < k_room_dist[rb][(uint8_t)tr]) ? ra : rb;
        const uint8_t *b = k_room_bounds[nr];
        *wx = (uint8_t)((b[0] + b[2]) >> 1);
        *wy = (uint8_t)((b[1] + b[3]) >> 1);
    }
}

/* Try the two axes that reduce Manhattan distance before falling back to BFS. */
static uint8_t greedy_step_to(Sim *s, uint8_t id, uint8_t tx, uint8_t ty) {
    Agent *a = &s->agents[id];
    int8_t dirs[4];
    uint8_t n = 0u, i;
    int16_t dx = (int16_t)tx - a->x, dy = (int16_t)ty - a->y;

    if (abs_u8_diff(a->x, tx) >= abs_u8_diff(a->y, ty)) {
        if (dx) dirs[n++] = (dx > 0) ? 1 : 3;
        if (dy) dirs[n++] = (dy > 0) ? 2 : 0;
    } else {
        if (dy) dirs[n++] = (dy > 0) ? 2 : 0;
        if (dx) dirs[n++] = (dx > 0) ? 1 : 3;
    }
    /* Side steps let actors skirt a desk/pillar without a full-map search. */
    for (i = 0u; i < 4u; ++i) {
        uint8_t d = i;
        uint8_t seen = 0u, j;
        for (j = 0u; j < n; ++j) if ((uint8_t)dirs[j] == d) { seen = 1u; break; }
        if (!seen) dirs[n++] = (int8_t)d;
    }

    for (i = 0u; i < n; ++i) {
        uint8_t d = (uint8_t)dirs[i];
        int16_t nx_i = (int16_t)a->x + k_dx[d], ny_i = (int16_t)a->y + k_dy[d];
        uint8_t nx, ny; uint16_t ci;
        if (!in_bounds_i(nx_i, ny_i)) continue;
        nx = (uint8_t)nx_i; ny = (uint8_t)ny_i;
        if (!sim_cell_passable(s, nx, ny, 1u)) continue;
        if (occupied_by_other(s, id, nx, ny)) continue;
        ci = cell_index(nx, ny);
        (void)ci;
        if (sim_cell_kind(s, nx, ny) == CELL_DOOR_CLOSED) {
            int8_t door = door_at(nx, ny);
            if (door >= 0) s->door_open_mask |= (uint8_t)(1u << (uint8_t)door);
            record_event(s, EVENT_DOOR, nx, ny, sim_agent_team(id));
            invalidate_route(a);
            return 2u;
        }
        a->x = nx; a->y = ny;
        invalidate_route(a);
        record_event(s, EVENT_MOVE, nx, ny, sim_agent_team(id));
        return 1u;
    }
    return 0u;
}

void sim_do_move_or_door(Sim *s, uint8_t id, uint8_t tx, uint8_t ty) {
    Agent *a = &s->agents[id];
    uint8_t old_x = a->x, old_y = a->y;
    uint8_t wx, wy, r;
    room_waypoint(a, tx, ty, &wx, &wy);
    if (greedy_step_to(s, id, wx, wy)) return;
    r = follow_route(s, id, wx, wy);
    if (r == 2u) {
        uint8_t d = a->route[a->route_pos];
        uint8_t nx = (uint8_t)((int16_t)a->x + k_dx[d]);
        uint8_t ny = (uint8_t)((int16_t)a->y + k_dy[d]);
        record_event(s, EVENT_DOOR, nx, ny, sim_agent_team(id));
    } else if (r == 1u && (a->x != old_x || a->y != old_y)) {
        record_event(s, EVENT_MOVE, a->x, a->y, sim_agent_team(id));
    }
}

uint8_t sim_target_visible(const Sim *s, uint8_t id, uint8_t target_id) {
    const Agent *a;
    const Agent *e;
    if (id >= s->agent_count || target_id >= s->agent_count) return 0u;
    a = &s->agents[id]; e = &s->agents[target_id];
    if (!a->alive || !e->alive) return 0u;
    return sim_has_los(s, a->x, a->y, e->x, e->y);
}

uint8_t sim_target_in_range(const Sim *s, uint8_t id, uint8_t target_id) {
    const Agent *a;
    const Agent *e;
    if (id >= s->agent_count || target_id >= s->agent_count) return 0u;
    a = &s->agents[id]; e = &s->agents[target_id];
    return dist2(a->x, a->y, e->x, e->y) <= (SIM_SHOT_RANGE * SIM_SHOT_RANGE);
}

void sim_patrol_target(const Sim *s, uint8_t id, uint8_t *tx, uint8_t *ty) {
    const Agent *a = &s->agents[id];
    uint8_t li = local_index(id);
    if (sim_agent_team(id) == TEAM_BLUE) {
        *tx = k_blue_patrol_x[li]; *ty = k_blue_patrol_y[li];
    } else {
        *tx = k_red_patrol_x[li]; *ty = k_red_patrol_y[li];
    }
    if (a->x == *tx && a->y == *ty) { *tx = a->home_x; *ty = a->home_y; }
}

void sim_record_reload(Sim *s, uint8_t id) {
    Agent *a = &s->agents[id];
    record_event(s, EVENT_RELOAD, a->x, a->y, sim_agent_team(id));
}

void sim_init(Sim *s, uint16_t seed) {
    uint8_t i;
    s->tick = 0u;
    s->rng = seed ? seed : 1u;
    s->done = 0u;
    s->winner = TEAM_DRAW;
    s->event_count = 0u;
    s->last_acted_mask = 0u;
    s->total_replans = 0u;
    s->total_plan_reuses = 0u;
    /* Door 5 (Security <-> Service) starts open in the authored map. */
    s->door_open_mask = (uint8_t)(1u << 5u);

    for (i = 0u; i < SIM_MAX_AGENTS; ++i) {
        Agent *a = &s->agents[i];
        uint8_t j;
        a->x = a->y = a->home_x = a->home_y = 0u;
        a->hp = 0u; a->ammo = 0u; a->alive = 0u; a->suppressed = 0u; a->hit_flash = 0u;
        a->last_seen_x = a->last_seen_y = 0u; a->last_seen_valid = 0u;
        a->last_seen_enemy = NO_AGENT; a->last_seen_tick = 0u; a->goal = GOAL_PATROL;
        a->plan_goal = GOAL_IDLE; a->plan_target = NO_AGENT; a->plan_tx = a->plan_ty = 0u;
        a->plan_len = a->plan_pos = a->plan_age = 0u; a->facts = 0u;
        a->replan_count = a->reuse_count = 0u;
        for (j = 0u; j < SIM_PLAN_MAX; ++j) a->plan[j] = ACTION_NONE;
        a->route_tx = a->route_ty = a->route_len = a->route_pos = 0u;
        for (j = 0u; j < SIM_ROUTE_STEPS; ++j) a->route[j] = 0u;
    }

    /* Five-to-seven hostiles, like the source sim. Seed 42 yields six here. */
    s->red_count = (uint8_t)(SIM_RED_MIN + (uint8_t)(rng_next(s) % (SIM_RED_MAX - SIM_RED_MIN + 1u)));
    s->agent_count = (uint8_t)(SIM_BLUE_COUNT + s->red_count);

    for (i = 0u; i < SIM_BLUE_COUNT; ++i) init_agent(s, i, 0u);
    for (i = 0u; i < s->red_count; ++i) {
        uint8_t room = rng_range_u8(s, 1u, (uint8_t)(SIM_ROOM_COUNT - 1u));
        init_agent(s, (uint8_t)(SIM_BLUE_COUNT + i), room);
    }
}

void sim_tick(Sim *s) {
    uint8_t i, first_team;
    uint8_t slots;
    if (s->done) return;
    ++s->tick;
    s->event_count = 0u;
    s->last_acted_mask = 0u;
    for (i = 0u; i < s->agent_count; ++i) {
        if (s->agents[i].suppressed) --s->agents[i].suppressed;
        if (s->agents[i].hit_flash) --s->agents[i].hit_flash;
    }

    /* Every living actor gets one decision/action in each logical tick.
     * One banked call handles the whole round, avoiding eleven mapper swaps. */
    first_team = (uint8_t)(s->tick & 1u);
    slots = (s->red_count > SIM_BLUE_COUNT) ? s->red_count : SIM_BLUE_COUNT;
    brain_run_actor_round(s, first_team, slots);

    check_winner(s);
    if (s->tick >= SIM_TICK_CAP && !s->done) {
        uint8_t bhp = sim_team_hp(s, TEAM_BLUE);
        uint8_t rhp = sim_team_hp(s, TEAM_RED);
        s->done = 1u;
        if (bhp > rhp) s->winner = TEAM_BLUE;
        else if (rhp > bhp) s->winner = TEAM_RED;
        else s->winner = TEAM_DRAW;
    }
}

const char *sim_goal_name(GoalKind goal) {
    switch (goal) {
        case GOAL_ESCAPE_DANGER: return "ESCAPE";
        case GOAL_COVER: return "COVER";
        case GOAL_KILL_ENEMY: return "KILL";
        case GOAL_SEARCH_LOST: return "SEARCH";
        case GOAL_PATROL: return "PATROL";
        case GOAL_IDLE: return "IDLE";
        default: return "?";
    }
}

const char *sim_action_name(ActionKind action) {
    switch (action) {
        case ACTION_RELOAD: return "RELOAD";
        case ACTION_MOVE_TO_TARGET: return "MOVE_TGT";
        case ACTION_SHOOT: return "SHOOT";
        case ACTION_MOVE_TO_COVER: return "MOVE_CVR";
        case ACTION_MOVE_TO_LAST_SEEN: return "MOVE_LAST";
        case ACTION_MOVE_PATROL: return "PATROL";
        case ACTION_WAIT: return "WAIT";
        default: return "NONE";
    }
}
