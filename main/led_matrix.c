#include "led_matrix.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "driver/rmt_tx.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"
#include "led_strip_rmt.h"

#include "board_config.h"
#include "rf_receiver.h"

static const char *TAG = "led_matrix";
static led_strip_handle_t s_strip;

/* --------------------------------------------------------------------------
 * Core Pixel & Framebuffer Helpers
 * -------------------------------------------------------------------------- */

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} rgb_color_t;

static rgb_color_t s_framebuffer[CONFIG_LED_MATRIX_WIDTH][CONFIG_LED_MATRIX_HEIGHT];

static uint32_t matrix_index(uint16_t x, uint16_t y)
{
    const uint16_t physical_y = (x & 1U)
        ? (CONFIG_LED_MATRIX_HEIGHT - 1U - y)
        : y;
    return ((uint32_t) x * CONFIG_LED_MATRIX_HEIGHT) + physical_y;
}

static uint8_t scale_brightness(uint8_t value)
{
    return (uint8_t) (((uint16_t) value * CONFIG_LED_BRIGHTNESS) / 100U);
}

static void hsv_to_rgb(uint8_t h, uint8_t s, uint8_t v, uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (s == 0) {
        *r = *g = *b = v;
        return;
    }
    uint8_t region = h / 43;
    uint8_t remainder = (h - (region * 43)) * 6;

    uint8_t p = (v * (255 - s)) >> 8;
    uint8_t q = (v * (255 - ((s * remainder) >> 8))) >> 8;
    uint8_t t = (v * (255 - ((s * (255 - remainder)) >> 8))) >> 8;

    switch (region) {
    case 0:  *r = v; *g = t; *b = p; break;
    case 1:  *r = q; *g = v; *b = p; break;
    case 2:  *r = p; *g = v; *b = t; break;
    case 3:  *r = p; *g = q; *b = v; break;
    case 4:  *r = t; *g = p; *b = v; break;
    default: *r = v; *g = p; *b = q; break;
    }
}

static void clear_framebuffer(void)
{
    for (uint16_t x = 0; x < CONFIG_LED_MATRIX_WIDTH; ++x) {
        for (uint16_t y = 0; y < CONFIG_LED_MATRIX_HEIGHT; ++y) {
            s_framebuffer[x][y].r = 0;
            s_framebuffer[x][y].g = 0;
            s_framebuffer[x][y].b = 0;
        }
    }
}

static void set_fb_pixel(int x, int y, uint8_t r, uint8_t g, uint8_t b)
{
    if (x >= 0 && x < CONFIG_LED_MATRIX_WIDTH && y >= 0 && y < CONFIG_LED_MATRIX_HEIGHT) {
        s_framebuffer[x][y].r = r;
        s_framebuffer[x][y].g = g;
        s_framebuffer[x][y].b = b;
    }
}

/* Five centered, crisp 5x5 silhouettes for the shape-recognition game. */
static uint8_t shape_mask(int shape, int y)
{
    static const uint8_t masks[5][8] = {
        /* Circle */
        {0x00, 0x18, 0x7c, 0x7c, 0x7c, 0x18, 0x00, 0x00},
        /* Square */
        {0x00, 0x7c, 0x7c, 0x7c, 0x7c, 0x7c, 0x00, 0x00},
        /* Triangle: one-pixel apex, then two wider steps */
        {0x00, 0x10, 0x38, 0x38, 0x7c, 0x7c, 0x00, 0x00},
        /* Heart */
        {0x00, 0x6c, 0x7c, 0x7c, 0x38, 0x10, 0x00, 0x00},
        /* Star */
        {0x00, 0x10, 0x7c, 0x38, 0x7c, 0x10, 0x00, 0x00},
    };

    if (shape < 0 || shape >= 5 || y < 0 || y >= 8) {
        return 0;
    }
    return masks[shape][y];
}

static void draw_shape(int shape, int origin_x, uint8_t r, uint8_t g, uint8_t b,
                       float pulse)
{
    /* Pulse the solid face itself so the outline stays sharp. */
    uint8_t face_r = (uint8_t) (r * (0.58f + 0.42f * pulse));
    uint8_t face_g = (uint8_t) (g * (0.58f + 0.42f * pulse));
    uint8_t face_b = (uint8_t) (b * (0.58f + 0.42f * pulse));
    for (int y = 0; y < 8; ++y) {
        uint8_t mask = shape_mask(shape, y);
        for (int x = 0; x < 8; ++x) {
            if ((mask & (1U << (7 - x))) != 0) {
                set_fb_pixel(origin_x + x, y, face_r, face_g, face_b);
            }
        }
    }

    /* A restrained anti-aliased corner keeps only the triangle readable. */
    if (shape == 2) {
        uint8_t edge_r = (uint8_t) (face_r * 0.22f);
        uint8_t edge_g = (uint8_t) (face_g * 0.22f);
        uint8_t edge_b = (uint8_t) (face_b * 0.22f);
        set_fb_pixel(origin_x + 2, 1, edge_r, edge_g, edge_b);
        set_fb_pixel(origin_x + 4, 1, edge_r, edge_g, edge_b);
        set_fb_pixel(origin_x + 1, 2, edge_r, edge_g, edge_b);
        set_fb_pixel(origin_x + 5, 2, edge_r, edge_g, edge_b);
    }
}

static void add_fb_pixel(int x, int y, uint8_t r, uint8_t g, uint8_t b)
{
    if (x >= 0 && x < CONFIG_LED_MATRIX_WIDTH && y >= 0 && y < CONFIG_LED_MATRIX_HEIGHT) {
        uint16_t nr = (uint16_t) s_framebuffer[x][y].r + r;
        uint16_t ng = (uint16_t) s_framebuffer[x][y].g + g;
        uint16_t nb = (uint16_t) s_framebuffer[x][y].b + b;
        s_framebuffer[x][y].r = (nr > 255) ? 255 : (uint8_t) nr;
        s_framebuffer[x][y].g = (ng > 255) ? 255 : (uint8_t) ng;
        s_framebuffer[x][y].b = (nb > 255) ? 255 : (uint8_t) nb;
    }
}

static esp_err_t flush_framebuffer(bool rf_active)
{
    for (uint16_t x = 0; x < CONFIG_LED_MATRIX_WIDTH; ++x) {
        for (uint16_t y = 0; y < CONFIG_LED_MATRIX_HEIGHT; ++y) {
            ESP_RETURN_ON_ERROR(
                led_matrix_set_xy(x, y,
                                  s_framebuffer[x][y].r,
                                  s_framebuffer[x][y].g,
                                  s_framebuffer[x][y].b),
                TAG, "failed to set pixel");
        }
    }

    if (rf_active) {
        ESP_RETURN_ON_ERROR(led_matrix_set_xy(0, 0, 0, 255, 0), TAG, "rf status pixel failed");
    }

    return led_matrix_refresh();
}

/* --------------------------------------------------------------------------
 * Particle & Ripple Systems
 * -------------------------------------------------------------------------- */

#define MAX_PARTICLES 64
typedef struct {
    bool active;
    float x, y;
    float vx, vy;
    float gravity;
    uint8_t r, g, b;
    int life;
    int max_life;
    bool sparkle;
} particle_t;

static particle_t s_particles[MAX_PARTICLES];

static void clear_particles(void)
{
    for (int i = 0; i < MAX_PARTICLES; ++i) {
        s_particles[i].active = false;
    }
}

static void spawn_particle_with_gravity(float x, float y, float vx, float vy,
                                        float gravity, uint8_t r, uint8_t g,
                                        uint8_t b, int life, bool sparkle)
{
    for (int i = 0; i < MAX_PARTICLES; ++i) {
        if (!s_particles[i].active) {
            s_particles[i].active = true;
            s_particles[i].x = x;
            s_particles[i].y = y;
            s_particles[i].vx = vx;
            s_particles[i].vy = vy;
            s_particles[i].gravity = gravity;
            s_particles[i].r = r;
            s_particles[i].g = g;
            s_particles[i].b = b;
            s_particles[i].life = life;
            s_particles[i].max_life = life;
            s_particles[i].sparkle = sparkle;
            break;
        }
    }
}

static void spawn_particle(float x, float y, float vx, float vy, uint8_t r,
                           uint8_t g, uint8_t b, int life, bool sparkle)
{
    spawn_particle_with_gravity(x, y, vx, vy, 0.045f,
                                r, g, b, life, sparkle);
}

static void spawn_firework_burst(float cx, float cy, int count)
{
    uint8_t burst_hue = esp_random() % 256;
    uint8_t base_r, base_g, base_b;
    hsv_to_rgb(burst_hue, 220, 255, &base_r, &base_g, &base_b);

    int actual_count = (count > 10) ? 9 : count;
    for (int i = 0; i < actual_count; ++i) {
        float angle = ((float) i / (float) actual_count) * 6.28318f;
        float speed = 0.5f + ((float) (esp_random() % 50) / 100.0f) * 0.7f;
        float vx = cosf(angle) * speed;
        float vy = sinf(angle) * speed * 0.5f;

        int life = 12 + (esp_random() % 10);
        bool sparkle = (esp_random() % 4 == 0);
        spawn_particle(cx, cy, vx, vy, base_r, base_g, base_b, life, sparkle);
    }
}

static void spawn_payoff_explosion(bool is_berry_bush)
{
    for (int i = 0; i < 45; ++i) {
        float cx = 27.0f + ((float) (esp_random() % 40) / 10.0f) - 2.0f;
        float cy = 3.0f + ((float) (esp_random() % 30) / 10.0f) - 1.5f;

        float angle = 3.14159f * (0.6f + ((float) (esp_random() % 100) / 100.0f) * 0.8f);
        float speed = 0.5f + ((float) (esp_random() % 100) / 100.0f) * 1.5f;
        float vx = cosf(angle) * speed - 0.4f;
        float vy = sinf(angle) * speed * 0.6f;

        uint8_t r, g, b;
        if (is_berry_bush) {
            if (esp_random() % 3 == 0) {
                hsv_to_rgb(esp_random() % 256, 220, 255, &r, &g, &b);
            } else if (esp_random() % 2 == 0) {
                r = 255; g = 20; b = 180;
            } else {
                r = 0; g = 220; b = 255;
            }
        } else {
            if (esp_random() % 3 == 0) {
                hsv_to_rgb(esp_random() % 256, 220, 255, &r, &g, &b);
            } else {
                uint8_t hue = esp_random() % 35;
                hsv_to_rgb(hue, 255, 255, &r, &g, &b);
            }
        }

        int life = 20 + (esp_random() % 25);
        bool sparkle = (esp_random() % 2 == 0);
        spawn_particle(cx, cy, vx, vy, r, g, b, life, sparkle);
    }
}

/* Magic Wand Ripple System */
#define MAX_RIPPLES 6
typedef struct {
    bool active;
    float cx, cy;
    float radius;
    float speed;
    uint8_t r, g, b;
} ripple_t;

static ripple_t s_ripples[MAX_RIPPLES];

static void spawn_ripple(float cx, float cy)
{
    for (int i = 0; i < MAX_RIPPLES; ++i) {
        if (!s_ripples[i].active) {
            s_ripples[i].active = true;
            s_ripples[i].cx = cx;
            s_ripples[i].cy = cy;
            s_ripples[i].radius = 0.2f;
            s_ripples[i].speed = 0.7f + ((float) (esp_random() % 50) / 100.0f);
            hsv_to_rgb(esp_random() % 256, 220, 255, &s_ripples[i].r, &s_ripples[i].g, &s_ripples[i].b);
            break;
        }
    }
}

/* --------------------------------------------------------------------------
 * Visual Toddler Toy Suite
 * -------------------------------------------------------------------------- */

typedef enum {
    GAME_1_STRETCHY_GIRAFFE = 0,
    GAME_2_FIREWORKS_WAND,
    GAME_3_ENCHANTED_FIRE,
    GAME_4_STRETCHY_REGENWURM,
    GAME_5_FULL_EMPTY,
    GAME_6_FIREFLY_CATCH,
    GAME_7_RED_GREEN_GIRAFFE,
    GAME_8_SHAPES,
    GAME_9_SPACESHIP,
} toy_state_t;

static void start_game_transition(toy_state_t next_state, toy_state_t *current_state,
                                  bool *wipe_active, int *wipe_ticks)
{
    *current_state = next_state;
    *wipe_active = true;
    *wipe_ticks = 0;
}

/* Starfield system for the warp-drive rocket game. */
#define NUM_STARS 14
typedef struct {
    float x;
    int y;
    float speed;
    uint8_t brightness;
    uint8_t hue;
} star_t;

static star_t s_stars[NUM_STARS];

static void init_starfield(void)
{
    for (int i = 0; i < NUM_STARS; ++i) {
        s_stars[i].x = (float) (esp_random() % CONFIG_LED_MATRIX_WIDTH);
        s_stars[i].y = (int) (esp_random() % CONFIG_LED_MATRIX_HEIGHT);
        s_stars[i].speed = 0.15f + ((float) (esp_random() % 30) / 100.0f);
        s_stars[i].brightness = 100 + (esp_random() % 155);
        s_stars[i].hue = (esp_random() % 4 == 0) ? 40 : 140;
    }
}

/* Chunky 6x5 rocket facing right. */
static void draw_rocket(int rx, int ry, bool thrust_active, float flicker)
{
    set_fb_pixel(rx + 5, ry + 2, 255, 255, 255);
    set_fb_pixel(rx + 4, ry + 1, 230, 40, 40);
    set_fb_pixel(rx + 4, ry + 2, 255, 255, 255);
    set_fb_pixel(rx + 4, ry + 3, 230, 40, 40);

    set_fb_pixel(rx + 3, ry + 1, 255, 255, 255);
    set_fb_pixel(rx + 3, ry + 2, 0, 230, 255);
    set_fb_pixel(rx + 3, ry + 3, 255, 255, 255);

    set_fb_pixel(rx + 2, ry + 0, 230, 30, 30);
    set_fb_pixel(rx + 2, ry + 1, 220, 220, 220);
    set_fb_pixel(rx + 2, ry + 2, 220, 220, 220);
    set_fb_pixel(rx + 2, ry + 3, 220, 220, 220);
    set_fb_pixel(rx + 2, ry + 4, 230, 30, 30);

    set_fb_pixel(rx + 1, ry + 1, 120, 120, 140);
    set_fb_pixel(rx + 1, ry + 2, 160, 160, 180);
    set_fb_pixel(rx + 1, ry + 3, 120, 120, 140);

    if (thrust_active) {
        int flame_len = 5 + (int) (flicker * 3.0f);
        for (int fx = 0; fx < flame_len; ++fx) {
            int px = rx - fx;
            uint8_t flame_g = (fx < 2) ? 240 : (uint8_t) (180 - fx * 25);
            uint8_t flame_b = (fx < 1) ? 200 : 0;
            set_fb_pixel(px, ry + 2, 255, flame_g, flame_b);
            if (fx < 3) {
                set_fb_pixel(px, ry + 1, 204, (uint8_t) (flame_g * 0.5f), 0);
                set_fb_pixel(px, ry + 3, 204, (uint8_t) (flame_g * 0.5f), 0);
            }
        }
    } else {
        uint8_t idle_glow = (uint8_t) (120 + flicker * 100);
        set_fb_pixel(rx, ry + 2, idle_glow, (uint8_t) (idle_glow * 0.5f), 20);
    }
}

/* ==========================================================================
 * Realistic fluid and wave physics system
 * ========================================================================== */
#define CUP_LEFT 10
#define CUP_RIGHT 21
#define CUP_BOTTOM 7
#define CUP_TOP 1
#define CUP_WIDTH (CUP_RIGHT - CUP_LEFT + 1)

typedef struct {
    float height[CUP_WIDTH];
    float velocity[CUP_WIDTH];
    float target_fill;
    float current_fill;
} fluid_cup_t;

static fluid_cup_t s_fluid;

static void init_fluid_cup(void)
{
    s_fluid.target_fill = 0.0f;
    s_fluid.current_fill = 0.0f;
    for (int i = 0; i < CUP_WIDTH; ++i) {
        s_fluid.height[i] = 0.0f;
        s_fluid.velocity[i] = 0.0f;
    }
}

static void fluid_splash_impact(int column, float force)
{
    if (column >= 0 && column < CUP_WIDTH) {
        s_fluid.velocity[column] -= force;
        if (column > 0) s_fluid.velocity[column - 1] -= force * 0.5f;
        if (column < CUP_WIDTH - 1) s_fluid.velocity[column + 1] -= force * 0.5f;
    }
}

static void update_fluid_physics(void)
{
    const float k_spring = 0.045f;
    const float k_spread = 0.22f;
    const float k_damping = 0.94f;

    s_fluid.current_fill += (s_fluid.target_fill - s_fluid.current_fill) * 0.12f;

    for (int i = 0; i < CUP_WIDTH; ++i) {
        float displacement = s_fluid.height[i] - s_fluid.current_fill;
        s_fluid.velocity[i] -= k_spring * displacement;
        s_fluid.height[i] += s_fluid.velocity[i];
        s_fluid.velocity[i] *= k_damping;
        if (s_fluid.height[i] < 0.0f) s_fluid.height[i] = 0.0f;
        if (s_fluid.height[i] > 6.5f) s_fluid.height[i] = 6.5f;
    }

    float left_deltas[CUP_WIDTH] = {0};
    float right_deltas[CUP_WIDTH] = {0};
    for (int pass = 0; pass < 2; ++pass) {
        for (int i = 0; i < CUP_WIDTH; ++i) {
            if (i > 0) {
                left_deltas[i] = k_spread * (s_fluid.height[i] - s_fluid.height[i - 1]);
                s_fluid.velocity[i - 1] += left_deltas[i];
            }
            if (i < CUP_WIDTH - 1) {
                right_deltas[i] = k_spread * (s_fluid.height[i] - s_fluid.height[i + 1]);
                s_fluid.velocity[i + 1] += right_deltas[i];
            }
        }
        for (int i = 0; i < CUP_WIDTH; ++i) {
            if (i > 0) s_fluid.height[i - 1] += left_deltas[i];
            if (i < CUP_WIDTH - 1) s_fluid.height[i + 1] += right_deltas[i];
        }
    }
}

/* ==========================================================================
 * Enchanted campfire and ember physics
 * ========================================================================== */
#define FIRE_WIDTH CONFIG_LED_MATRIX_WIDTH

typedef struct {
    uint8_t heat[FIRE_WIDTH];
    uint8_t target_heat[FIRE_WIDTH];
    float blaze_boost;
} fire_sim_t;

static fire_sim_t s_fire;

static void init_fire_sim(void)
{
    s_fire.blaze_boost = 1.0f;
    for (int x = 0; x < FIRE_WIDTH; ++x) {
        s_fire.heat[x] = 24 + (esp_random() % 30);
        s_fire.target_heat[x] = 26 + (esp_random() % 30);
    }
}

static void fire_heat_to_rgb(uint8_t temperature, uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (temperature == 0) {
        *r = 0;
        *g = 0;
        *b = 0;
    } else if (temperature < 96) {
        *r = (temperature * 255) / 96;
        *g = (temperature * 35) / 96;
        *b = 0;
    } else if (temperature < 192) {
        *r = 255;
        *g = 35 + (((temperature - 96) * 190) / 96);
        *b = (temperature - 96) / 8;
    } else {
        *r = 255;
        *g = 225 + (((temperature - 192) * 30) / 63);
        *b = 12 + (((temperature - 192) * 243) / 63);
    }
}

static void stoke_campfire(float heat_add)
{
    for (int x = 0; x < FIRE_WIDTH; ++x) {
        int heat = (int) s_fire.heat[x] + (int) heat_add + (int) (esp_random() % 8);
        s_fire.heat[x] = (heat > 255) ? 255 : (uint8_t) heat;
    }
}

static void spawn_campfire_embers(float cx, float cy, int count, float speed_scale)
{
    for (int i = 0; i < count; ++i) {
        float angle = 3.14159f *
            (0.15f + ((float) (esp_random() % 100) / 100.0f) * 0.70f);
        float speed = 0.18f +
            ((float) (esp_random() % 100) / 100.0f) * speed_scale;
        float vx = cosf(angle) * speed;
        float vy = -sinf(angle) * speed * 0.75f;
        uint8_t r, g, b;

        if (esp_random() % 5 == 0) {
            r = 255;
            g = 220;
            b = 55;
        } else {
            hsv_to_rgb(5 + (esp_random() % 31), 245, 255, &r, &g, &b);
        }
        spawn_particle(cx, cy, vx, vy, r, g, b,
                       24 + (esp_random() % 24), false);
    }
}

static void matrix_demo_task(void *argument)
{
    (void) argument;

    uint32_t last_rf_frame_count = 0;
    toy_state_t currentState = GAME_1_STRETCHY_GIRAFFE;
    toy_state_t nextCharacterGame = GAME_3_ENCHANTED_FIRE;

    /* Stretchy Parameters */
    float current_stretch_len = 0.0f;
    float target_stretch_len = 0.0f;
    int tap_count = 0;

    bool is_chomp_anim = false;
    int chomp_ticks = 0;
    bool is_chewing = false;
    int chew_ticks = 0;

    float boing_phase = 0.0f;
    float flicker_phase = 0.0f;
    int blink_timer = 0;

    /* Mode timing */
    TickType_t stateStartTick = xTaskGetTickCount();
    TickType_t lastInteractionTick = xTaskGetTickCount();

    /* Aurora Wave phases */
    float auroraPhaseA = 0.0f;
    float auroraPhaseB = 0.0f;

    /* Full / empty water cup */
    bool water_spill_anim = false;
    int water_spill_ticks = 0;
    float water_overflow_volume = 0.0f;

    /* Simple one-firefly catcher */
    int firefly_caught_count = 0;
    int firefly_flash_ticks = 0;
    float firefly_phase = 0.0f;

    /* Simple sleeping-giraffe peekaboo */
    bool giraffe_awake = false;
    int giraffe_awake_ticks = 0;
    int giraffe_peek_count = 0;

    /* Basic shapes and silhouette recognition */
    int shape_index = 0;
    float shape_pos = 12.0f;
    float shape_velocity = 0.08f;
    float shape_phase = 0.0f;
    int shape_pulse_ticks = 24;
    bool shape_finished = false;
    int shape_finish_ticks = 0;

    /* Warp-drive rocket ship */
    float ship_pos_x = 3.0f;
    float ship_target_x = 3.0f;
    float ship_bob_phase = 0.0f;
    int ship_boosts = 0;
    int ship_boost_ticks = 0;
    bool ship_warp_active = false;
    int ship_warp_ticks = 0;
    init_starfield();

    /* Shared colorful curtain between game states. */
    bool transition_wipe_active = false;
    int transition_wipe_ticks = 0;

    init_fluid_cup();
    init_fire_sim();

    ESP_LOGI(TAG, "Starting 7-game toddler toy suite!");

    while (true) {
        const TickType_t now = xTaskGetTickCount();
        const uint32_t current_rf_frame_count = rf_receiver_frame_count();
        bool buttonPressed = false;

        if (current_rf_frame_count != last_rf_frame_count) {
            last_rf_frame_count = current_rf_frame_count;
            if (!transition_wipe_active) {
                buttonPressed = true;
                lastInteractionTick = now;
                ESP_LOGI(TAG, "RF Button Pressed! Game State: %d", (int) currentState);
            }
        }

        flicker_phase += 0.15f;
        boing_phase += 0.3f;

        switch (currentState) {

        /* ==================================================================
         * GAME 1: THE STRETCHY GIRAFFE & APPLE TREE
         * ================================================================== */
        case GAME_1_STRETCHY_GIRAFFE: {
            clear_framebuffer();

            if (buttonPressed && !is_chomp_anim && !is_chewing) {
                tap_count++;
                target_stretch_len = (float) tap_count * 5.5f;
                boing_phase = 0.0f;
                spawn_particle(5.0f + current_stretch_len, 3.0f, 0.3f, -0.2f, 255, 255, 180, 8, true);

                if (target_stretch_len >= 21.0f) {
                    target_stretch_len = 21.0f;
                    is_chomp_anim = true;
                    chomp_ticks = 15;
                }
            }

            if (is_chewing) {
                target_stretch_len = 0.0f;
                current_stretch_len += (target_stretch_len - current_stretch_len) * 0.18f;
                chew_ticks--;
                if (chew_ticks <= 0) {
                    is_chewing = false;
                    tap_count = 0;
                    current_stretch_len = 0.0f;
                    target_stretch_len = 0.0f;
                    nextCharacterGame = GAME_3_ENCHANTED_FIRE;
                    start_game_transition(GAME_2_FIREWORKS_WAND, &currentState,
                                          &transition_wipe_active, &transition_wipe_ticks);
                    stateStartTick = now;
                    ESP_LOGI(TAG, "Giraffe tree chomp complete -> GAME_2_FIREWORKS_WAND!");
                }
            } else if (is_chomp_anim) {
                current_stretch_len += (21.0f - current_stretch_len) * 0.4f;
                chomp_ticks--;
                if (chomp_ticks <= 0) {
                    is_chomp_anim = false;
                    is_chewing = true;
                    chew_ticks = 42;
                    spawn_payoff_explosion(false);
                }
            } else {
                current_stretch_len += (target_stretch_len - current_stretch_len) * 0.25f;
            }

            float boing_offset = 0.0f;
            if (fabsf(target_stretch_len - current_stretch_len) > 0.3f || boing_phase < 6.28f) {
                boing_offset = sinf(boing_phase) * 0.65f;
            }

            float flicker1 = 0.6f + sinf(flicker_phase) * 0.4f;
            float flicker2 = 0.6f + cosf(flicker_phase * 1.3f) * 0.4f;

            /* Tree (Right Side) */
            set_fb_pixel(28, 4, 130, 65, 15); set_fb_pixel(29, 4, 110, 55, 10);
            set_fb_pixel(28, 5, 130, 65, 15); set_fb_pixel(29, 5, 110, 55, 10);
            set_fb_pixel(28, 6, 130, 65, 15); set_fb_pixel(29, 6, 110, 55, 10);
            set_fb_pixel(28, 7, 130, 65, 15); set_fb_pixel(29, 7, 110, 55, 10);

            for (int tx = 25; tx <= 31; ++tx) {
                for (int ty = 0; ty <= 3; ++ty) {
                    if ((tx == 25 && ty == 0) || (tx == 31 && ty == 0)) continue;
                    set_fb_pixel(tx, ty, 15, 180, 45);
                }
            }
            set_fb_pixel(26, 0, 40, 220, 70); set_fb_pixel(27, 0, 40, 220, 70);
            set_fb_pixel(28, 0, 40, 220, 70); set_fb_pixel(29, 0, 40, 220, 70); set_fb_pixel(30, 0, 40, 220, 70);

            set_fb_pixel(26, 1, (uint8_t) (255 * flicker1), 20, 30);
            set_fb_pixel(30, 1, (uint8_t) (255 * flicker2), 30, 20);
            set_fb_pixel(28, 2, (uint8_t) (255 * flicker1), (uint8_t) (220 * flicker2), 0);
            set_fb_pixel(27, 3, (uint8_t) (255 * flicker2), 15, 25);
            set_fb_pixel(29, 3, (uint8_t) (240 * flicker2), (uint8_t) (200 * flicker1), 10);

            /* Giraffe Body */
            set_fb_pixel(0, 6, 230, 160, 0); set_fb_pixel(0, 7, 120, 60, 10);
            set_fb_pixel(1, 6, 230, 160, 0); set_fb_pixel(1, 7, 120, 60, 10);
            set_fb_pixel(4, 6, 230, 160, 0); set_fb_pixel(4, 7, 120, 60, 10);
            set_fb_pixel(5, 6, 230, 160, 0); set_fb_pixel(5, 7, 120, 60, 10);

            for (int bx = 0; bx <= 5; ++bx) {
                for (int by = 3; by <= 5; ++by) {
                    set_fb_pixel(bx, by, 245, 175, 0);
                }
            }
            set_fb_pixel(1, 3, 150, 65, 10); set_fb_pixel(3, 4, 150, 65, 10);
            set_fb_pixel(0, 5, 150, 65, 10); set_fb_pixel(4, 5, 150, 65, 10);

            set_fb_pixel(4, 2, 245, 175, 0); set_fb_pixel(5, 2, 245, 175, 0);

            int neck_end_x = 5 + (int) current_stretch_len;
            for (int nx = 5; nx <= neck_end_x; ++nx) {
                int ny = 2 + (int) (boing_offset * sinf((float) (nx - 5) * 0.4f));
                if (ny < 1) ny = 1;
                if (ny > 3) ny = 3;
                if ((nx % 3) == 0) set_fb_pixel(nx, ny, 160, 70, 10);
                else set_fb_pixel(nx, ny, 245, 175, 0);
            }

            int hx = neck_end_x;
            int hy = 1 + (int) boing_offset;
            if (hy < 0) hy = 0;
            if (hy > 2) hy = 2;

            set_fb_pixel(hx, hy - 1, 160, 70, 10); set_fb_pixel(hx + 1, hy - 1, 160, 70, 10);
            set_fb_pixel(hx, hy, 255, 190, 0); set_fb_pixel(hx + 1, hy, 255, 190, 0);

            if (is_chomp_anim) {
                set_fb_pixel(hx, hy, 255, 255, 255); set_fb_pixel(hx + 1, hy, 255, 255, 200);
                set_fb_pixel(hx, hy + 1, 255, 230, 100); set_fb_pixel(hx + 1, hy + 1, 255, 255, 255);
            } else if (is_chewing) {
                uint8_t chew_g = (uint8_t) (180 + sinf(boing_phase * 2.0f) * 60.0f);
                set_fb_pixel(hx, hy, 255, chew_g, 0);
                set_fb_pixel(hx + 1, hy, 255, 255, 255);
                set_fb_pixel(hx + 1, hy + 1, 255, 100, 120);
            } else {
                blink_timer++;
                if (blink_timer % 90 > 82) set_fb_pixel(hx + 1, hy, 160, 70, 10);
                else set_fb_pixel(hx + 1, hy, 255, 255, 255);
            }

            if ((now - lastInteractionTick) > pdMS_TO_TICKS(14000)) {
                lastInteractionTick = now;
                nextCharacterGame = GAME_3_ENCHANTED_FIRE;
                start_game_transition(GAME_2_FIREWORKS_WAND, &currentState,
                                      &transition_wipe_active, &transition_wipe_ticks);
                stateStartTick = now;
            }
            break;
        }

        /* ==================================================================
         * GAME 2: FIREWORKS & MAGIC SPARKLE WAND (Celebration & Freeplay)
         * ================================================================== */
        case GAME_2_FIREWORKS_WAND: {
            auroraPhaseA += 0.04f;
            auroraPhaseB += 0.02f;

            /* Breathing Aurora background */
            for (uint16_t x = 0; x < CONFIG_LED_MATRIX_WIDTH; ++x) {
                for (uint16_t y = 0; y < CONFIG_LED_MATRIX_HEIGHT; ++y) {
                    float v1 = sinf(x * 0.25f + auroraPhaseA);
                    float v2 = sinf(y * 0.60f + auroraPhaseB);
                    float combined = (v1 + v2) * 0.5f;

                    uint8_t hue = (uint8_t) (120.0f + combined * 60.0f);
                    uint8_t val = (uint8_t) (40.0f + (v1 * 30.0f));
                    uint8_t r, g, b;
                    hsv_to_rgb(hue, 220, val, &r, &g, &b);
                    set_fb_pixel(x, y, r, g, b);
                }
            }

            /* Auto-launch 1 rocket every ~2.5s */
            if (esp_random() % 75 == 0) {
                float rx = (float) ((esp_random() % (CONFIG_LED_MATRIX_WIDTH - 8)) + 4);
                float ry = (float) ((esp_random() % 4) + 1);
                spawn_firework_burst(rx, ry, 9);
            }

            if (buttonPressed) {
                float rx = (float) (esp_random() % (CONFIG_LED_MATRIX_WIDTH - 6) + 3);
                float ry = (float) (esp_random() % (CONFIG_LED_MATRIX_HEIGHT - 2) + 1);
                spawn_firework_burst(rx, ry, 9);
                spawn_ripple(rx, ry);
            }

            for (int i = 0; i < MAX_RIPPLES; ++i) {
                if (s_ripples[i].active) {
                    s_ripples[i].radius += s_ripples[i].speed;

                    for (int x = 0; x < CONFIG_LED_MATRIX_WIDTH; ++x) {
                        for (int y = 0; y < CONFIG_LED_MATRIX_HEIGHT; ++y) {
                            float dx = (float) x - s_ripples[i].cx;
                            float dy = ((float) y - s_ripples[i].cy) * 2.0f;
                            float dist = sqrtf(dx * dx + dy * dy);

                            if (fabsf(dist - s_ripples[i].radius) < 1.1f) {
                                if (esp_random() % 3 == 0) {
                                    add_fb_pixel(x, y, 255, 255, 255);
                                } else {
                                    add_fb_pixel(x, y, s_ripples[i].r, s_ripples[i].g, s_ripples[i].b);
                                }
                            }
                        }
                    }

                    if (s_ripples[i].radius > 32.0f) {
                        s_ripples[i].radius = 0.0f;
                        s_ripples[i].active = false;
                    }
                }
            }

            if ((now - stateStartTick) > pdMS_TO_TICKS(8000)) {
                if (nextCharacterGame == GAME_3_ENCHANTED_FIRE) {
                    clear_particles();
                    for (int i = 0; i < MAX_RIPPLES; ++i) {
                        s_ripples[i].active = false;
                    }
                    init_fire_sim();
                }
                start_game_transition(nextCharacterGame, &currentState,
                                      &transition_wipe_active, &transition_wipe_ticks);
                current_stretch_len = 0.0f;
                target_stretch_len = 0.0f;
                tap_count = 0;
                lastInteractionTick = now;
                ESP_LOGI(TAG, "Fireworks/Wand complete -> Next Game state %d!", (int) currentState);
            }
            break;
        }

        /* ==================================================================
         * GAME 3: THE ENCHANTED CAMPFIRE
         * ================================================================== */
        case GAME_3_ENCHANTED_FIRE: {
            clear_framebuffer();

            static int fire_combo_hits = 0;
            static TickType_t last_fire_tap = 0;
            static int raging_fire_ticks = 0;

            if (buttonPressed) {
                if ((now - last_fire_tap) < pdMS_TO_TICKS(650)) {
                    fire_combo_hits++;
                } else {
                    fire_combo_hits = 1;
                }
                last_fire_tap = now;

                if (fire_combo_hits >= 4) {
                    fire_combo_hits = 0;
                    raging_fire_ticks = 90;
                    stoke_campfire(65.0f);
                    s_fire.blaze_boost = 2.15f;

                    /* The payoff is a sustained roaring flame with a dense
                     * lift of warm embers, never a firework explosion. */
                    spawn_campfire_embers(8.0f, 6.5f, 8, 0.90f);
                    spawn_campfire_embers(16.0f, 6.5f, 10, 1.05f);
                    spawn_campfire_embers(24.0f, 6.5f, 8, 0.90f);
                } else {
                    stoke_campfire(18.0f);
                    s_fire.blaze_boost += 0.18f;
                    if (s_fire.blaze_boost > 1.85f) s_fire.blaze_boost = 1.85f;

                    float ember_x = (float) (3 +
                        (esp_random() % (CONFIG_LED_MATRIX_WIDTH - 6)));
                    spawn_campfire_embers(ember_x, 6.5f, 10, 0.72f);
                }
            }

            /* A few embers always breathe out of the low idle fire. */
            if (esp_random() % 18 == 0) {
                float ember_x = (float) (2 +
                    (esp_random() % (CONFIG_LED_MATRIX_WIDTH - 4)));
                spawn_campfire_embers(ember_x, 6.5f, 1, 0.42f);
            }

            if (raging_fire_ticks > 0) {
                raging_fire_ticks--;
                if (raging_fire_ticks % 4 == 0) {
                    float ember_x = (float) (2 +
                        (esp_random() % (CONFIG_LED_MATRIX_WIDTH - 4)));
                    spawn_campfire_embers(ember_x, 6.5f, 3, 0.82f);
                }
            }

            /* Let the fire settle slowly so each tap produces a clear,
             * cumulative increase in flame height. */
            s_fire.blaze_boost += (1.0f - s_fire.blaze_boost) * 0.012f;

            for (int x = 0; x < FIRE_WIDTH; ++x) {
                if (esp_random() % 8 == 0) {
                    s_fire.target_heat[x] = 26 + (esp_random() % 38);
                }
                s_fire.heat[x] +=
                    ((int) s_fire.target_heat[x] - (int) s_fire.heat[x]) / 12;

                for (int y = 0; y < CONFIG_LED_MATRIX_HEIGHT; ++y) {
                    int dist_from_bottom = CONFIG_LED_MATRIX_HEIGHT - 1 - y;
                    float turbulence = sinf(flicker_phase * 1.6f + (float) x * 0.7f +
                                            (float) y * 1.1f) * 15.0f;
                    int flame_temp = (int) (s_fire.heat[x] * s_fire.blaze_boost) -
                                     (dist_from_bottom * 42) + (int) turbulence;
                    if (flame_temp < 0) flame_temp = 0;
                    if (flame_temp > 255) flame_temp = 255;

                    uint8_t fr, fg, fb;
                    fire_heat_to_rgb((uint8_t) flame_temp, &fr, &fg, &fb);
                    set_fb_pixel(x, y, fr, fg, fb);
                }

                uint8_t log_glow = (uint8_t) (42 +
                    sinf(flicker_phase * 0.8f + x * 0.3f) * 18);
                add_fb_pixel(x, CONFIG_LED_MATRIX_HEIGHT - 1,
                             log_glow, (uint8_t) (log_glow * 0.35f), 0);
                if ((x + (int) flicker_phase) % 7 == 0) {
                    add_fb_pixel(x, CONFIG_LED_MATRIX_HEIGHT - 1, 80, 20, 0);
                }
            }

            if ((now - lastInteractionTick) > pdMS_TO_TICKS(18000)) {
                lastInteractionTick = now;
                nextCharacterGame = GAME_4_STRETCHY_REGENWURM;
                start_game_transition(GAME_2_FIREWORKS_WAND, &currentState,
                                      &transition_wipe_active, &transition_wipe_ticks);
                stateStartTick = now;
            }
            break;
        }

        /* ==================================================================
         * GAME 4: THE STRETCHY REGENWURM & BERRY BUSH
         * ================================================================== */
        case GAME_4_STRETCHY_REGENWURM: {
            clear_framebuffer();

            if (buttonPressed && !is_chomp_anim && !is_chewing) {
                tap_count++;
                target_stretch_len = (float) tap_count * 5.5f;
                boing_phase = 0.0f;
                spawn_particle(5.0f + current_stretch_len, 3.0f, 0.3f, -0.2f, 255, 255, 180, 8, true);

                if (target_stretch_len >= 21.0f) {
                    target_stretch_len = 21.0f;
                    is_chomp_anim = true;
                    chomp_ticks = 15;
                }
            }

            if (is_chewing) {
                target_stretch_len = 0.0f;
                current_stretch_len += (target_stretch_len - current_stretch_len) * 0.18f;
                chew_ticks--;
                if (chew_ticks <= 0) {
                    is_chewing = false;
                    tap_count = 0;
                    current_stretch_len = 0.0f;
                    target_stretch_len = 0.0f;
                    nextCharacterGame = GAME_5_FULL_EMPTY;
                    start_game_transition(GAME_2_FIREWORKS_WAND, &currentState,
                                          &transition_wipe_active, &transition_wipe_ticks);
                    stateStartTick = now;
                    ESP_LOGI(TAG, "Regenwurm berry chomp complete -> GAME_2_FIREWORKS_WAND!");
                }
            } else if (is_chomp_anim) {
                current_stretch_len += (21.0f - current_stretch_len) * 0.4f;
                chomp_ticks--;
                if (chomp_ticks <= 0) {
                    is_chomp_anim = false;
                    is_chewing = true;
                    chew_ticks = 42;
                    spawn_payoff_explosion(true);
                }
            } else {
                current_stretch_len += (target_stretch_len - current_stretch_len) * 0.25f;
            }

            float boing_offset = 0.0f;
            if (fabsf(target_stretch_len - current_stretch_len) > 0.3f || boing_phase < 6.28f) {
                boing_offset = sinf(boing_phase) * 0.65f;
            }

            float flicker1 = 0.6f + sinf(flicker_phase) * 0.4f;
            float flicker2 = 0.6f + cosf(flicker_phase * 1.3f) * 0.4f;

            /* Berry Bush */
            for (int bx = 25; bx <= 31; ++bx) {
                for (int by = 1; by <= 5; ++by) {
                    if ((bx == 25 && by == 1) || (bx == 31 && by == 1)) continue;
                    set_fb_pixel(bx, by, 10, 140, 50);
                }
            }
            set_fb_pixel(26, 2, (uint8_t) (255 * flicker1), 20, 160);
            set_fb_pixel(27, 2, (uint8_t) (255 * flicker1), 20, 160);
            set_fb_pixel(30, 2, 0, (uint8_t) (220 * flicker2), 255);
            set_fb_pixel(28, 4, (uint8_t) (255 * flicker2), (uint8_t) (210 * flicker1), 0);
            set_fb_pixel(29, 4, (uint8_t) (255 * flicker2), (uint8_t) (210 * flicker1), 0);

            /* Regenwurm (Dynamic Unattached Crawling & Wiggling) */
            float tail_x = 1.0f + sinf(flicker_phase * 0.35f) * 4.0f;
            if (current_stretch_len > 1.0f) {
                tail_x = 1.0f + (1.0f - (current_stretch_len / 21.0f)) * 3.0f;
            }

            float head_x = tail_x + 6.0f + current_stretch_len;
            int tx = (int) (tail_x + 0.5f);
            int hx = (int) (head_x + 0.5f);
            if (hx > 25) hx = 25;
            if (tx < 1) tx = 1;

            for (int nx = tx; nx <= hx; ++nx) {
                float norm_x = (float) (nx - tx) / (float) (hx - tx > 0 ? hx - tx : 1);
                float wave = sinf(flicker_phase * 1.8f + (float) nx * 0.45f) * 1.0f + boing_offset * 1.2f;
                int wy = (int) (3.5f + wave);

                if (wy < 1) wy = 1;
                if (wy > 6) wy = 6;

                uint8_t r = 255;
                uint8_t g = 130;
                uint8_t b = 150;

                bool is_clitellum = (norm_x >= 0.28f && norm_x <= 0.42f);
                if (is_clitellum) {
                    r = 255; g = 200; b = 210;
                } else if (nx % 2 == 0) {
                    g = 110; b = 130;
                }

                set_fb_pixel(nx, wy, r, g, b);
                if (wy + 1 <= 6) {
                    set_fb_pixel(nx, wy + 1, (uint8_t) ((r * 3) / 4), (uint8_t) ((g * 3) / 4), (uint8_t) ((b * 3) / 4));
                }
            }

            float head_wave = sinf(flicker_phase * 1.5f + (float) hx * 0.35f) * 0.6f + boing_offset * 1.2f;
            int hy = (int) (3.5f + head_wave);
            if (hy < 1) hy = 1;
            if (hy > 5) hy = 5;

            if (is_chomp_anim) {
                set_fb_pixel(hx, hy, 255, 255, 255);
                set_fb_pixel(hx + 1, hy, 255, 255, 220);
                set_fb_pixel(hx, hy + 1, 255, 220, 100);
                set_fb_pixel(hx + 1, hy + 1, 255, 255, 255);
            } else if (is_chewing) {
                set_fb_pixel(hx, hy, 255, 180, 200);
                set_fb_pixel(hx + 1, hy, 255, 255, 255);
                set_fb_pixel(hx + 1, hy + 1, 255, 100, 150);
            } else {
                set_fb_pixel(hx, hy, 255, 160, 180);
                set_fb_pixel(hx, hy + 1, 255, 100, 150);
                blink_timer++;
                if (blink_timer % 90 > 82) {
                    set_fb_pixel(hx + 1, hy, 160, 70, 10);
                } else {
                    set_fb_pixel(hx + 1, hy, 255, 255, 255);
                }
            }

            if ((now - lastInteractionTick) > pdMS_TO_TICKS(14000)) {
                lastInteractionTick = now;
                nextCharacterGame = GAME_5_FULL_EMPTY;
                start_game_transition(GAME_2_FIREWORKS_WAND, &currentState,
                                      &transition_wipe_active, &transition_wipe_ticks);
                stateStartTick = now;
            }
            break;
        }

        /* ==================================================================
         * GAME 5: REALISTIC HYDRODYNAMIC WATER CUP & CASCADE SPLASH
         * ================================================================== */
        case GAME_5_FULL_EMPTY: {
            clear_framebuffer();
            update_fluid_physics();

            /* Button tap: pour a gravity-driven stream into the cup. */
            if (buttonPressed && !water_spill_anim) {
                const float rim_level = 5.6f;
                const float pour_amount = 1.4f;
                float requested_fill = s_fluid.target_fill + pour_amount;

                if (requested_fill > rim_level) {
                    water_overflow_volume += requested_fill - rim_level;
                    s_fluid.target_fill = rim_level;
                } else {
                    s_fluid.target_fill = requested_fill;
                }

                for (int drop = 0; drop < 7; ++drop) {
                    float drop_x = 14.5f + ((float) (esp_random() % 30) - 15.0f) * 0.1f;
                    float drop_vy = 0.45f + ((float) (esp_random() % 30) / 100.0f);
                    spawn_particle(drop_x, -1.0f - (float) drop * 0.6f,
                                   0.0f, drop_vy, 180, 240, 255, 18, true);
                }

                fluid_splash_impact(5, 0.9f);
                fluid_splash_impact(6, 0.9f);

                if (s_fluid.current_fill > 0.8f) {
                    for (int bubble = 0; bubble < 3; ++bubble) {
                        float bx = 12.0f + (float) (esp_random() % 8);
                        float by = (float) CUP_BOTTOM - (s_fluid.current_fill * 0.5f);
                        spawn_particle(bx, by, 0.0f, -0.16f,
                                       220, 255, 255, 14, true);
                    }
                }

                /* The bucket fills normally first. Only water added beyond
                 * the rim starts the overflow sequence. */
                if (water_overflow_volume > 0.01f) {
                    water_spill_anim = true;
                    water_spill_ticks = 80;
                }
            }

            /* Overflow stages: excess water swells, spills, then settles. */
            if (water_spill_anim) {
                const float rim_level = 5.6f;
                water_spill_ticks--;

                if (water_spill_ticks > 65) {
                    /* Added water briefly forms a surface-tension dome. */
                    float dome_height = water_overflow_volume * 0.5f;
                    if (dome_height > 0.7f) dome_height = 0.7f;
                    s_fluid.target_fill = rim_level + dome_height;
                    for (int col = 0; col < CUP_WIDTH; ++col) {
                        s_fluid.velocity[col] +=
                            sinf(flicker_phase * 2.5f + col * 0.8f) * 0.08f;
                    }
                } else if (water_spill_ticks > 15) {
                    /* Spend only the excess volume. Water in the upright
                     * bucket settles at the rim and never drains away. */
                    s_fluid.target_fill +=
                        (rim_level - s_fluid.target_fill) * 0.16f;
                    water_overflow_volume -= 0.028f;
                    if (water_overflow_volume < 0.0f) {
                        water_overflow_volume = 0.0f;
                    }

                    if (water_overflow_volume > 0.01f && water_spill_ticks % 2 == 0) {
                        float left_speed = 0.50f +
                            ((float) (esp_random() % 35) / 100.0f);
                        float left_lift = 0.28f +
                            ((float) (esp_random() % 11) / 100.0f);
                        spawn_particle_with_gravity(
                            (float) (CUP_LEFT - 1), (float) CUP_TOP,
                            -left_speed, -left_lift, 0.075f,
                            160, 235, 255, 28, true);

                        float right_speed = 0.50f +
                            ((float) (esp_random() % 35) / 100.0f);
                        float right_lift = 0.28f +
                            ((float) (esp_random() % 11) / 100.0f);
                        spawn_particle_with_gravity(
                            (float) (CUP_RIGHT + 1), (float) CUP_TOP,
                            right_speed, -right_lift, 0.075f,
                            160, 235, 255, 28, true);
                    }
                } else {
                    /* The final drops stop while the bucket remains full. */
                    s_fluid.target_fill = rim_level;
                    if (water_spill_ticks == 14) {
                        spawn_ripple(16.0f, 6.0f);
                    }
                }
            }

            /* Background glass shadow, without artificial exterior streams. */
            for (int gy = CUP_TOP; gy <= CUP_BOTTOM; ++gy) {
                set_fb_pixel(CUP_LEFT - 1, gy, 12, 28, 50);
                set_fb_pixel(CUP_RIGHT + 1, gy, 12, 28, 50);
            }

            /* Water body and surface crest. */
            for (int col = 0; col < CUP_WIDTH; ++col) {
                int world_x = CUP_LEFT + col;
                float col_h = s_fluid.height[col];
                if (col_h < 0.05f) continue;
                if (col_h > 7.0f) col_h = 7.0f;

                float surface_y = (float) (CUP_BOTTOM - 1) - col_h + 1.0f;
                int top_y = (int) (surface_y + 0.5f);
                if (top_y < CUP_TOP - 1) top_y = CUP_TOP - 1;

                for (int y = CUP_BOTTOM - 1; y >= top_y; --y) {
                    if (y < 0 || y >= CONFIG_LED_MATRIX_HEIGHT) continue;
                    float depth_factor = (float) (CUP_BOTTOM - y) / 6.0f;
                    uint8_t wr, wg, wb;
                    if (y == top_y) {
                        float crest = 0.8f +
                            sinf(flicker_phase * 2.2f + col * 0.9f) * 0.2f;
                        wr = (uint8_t) (170 * crest);
                        wg = (uint8_t) (240 * crest);
                        wb = 255;
                    } else {
                        wr = (uint8_t) (10 + 30 * depth_factor);
                        wg = (uint8_t) (85 + 120 * depth_factor);
                        wb = (uint8_t) (185 + 70 * depth_factor);
                    }
                    set_fb_pixel(world_x, y, wr, wg, wb);
                }
            }

            /* Glass walls, rim lips, and bottom remain readable. */
            for (int gy = CUP_TOP; gy < CUP_BOTTOM; ++gy) {
                bool submerged = ((CUP_BOTTOM - gy) <=
                                  (int) (s_fluid.current_fill + 0.5f));
                if (submerged) {
                    set_fb_pixel(CUP_LEFT - 1, gy, 50, 170, 240);
                    set_fb_pixel(CUP_RIGHT + 1, gy, 50, 170, 240);
                } else {
                    set_fb_pixel(CUP_LEFT - 1, gy, 120, 175, 205);
                    set_fb_pixel(CUP_RIGHT + 1, gy, 120, 175, 205);
                }
            }

            set_fb_pixel(CUP_LEFT - 1, CUP_TOP, 220, 245, 255);
            set_fb_pixel(CUP_RIGHT + 1, CUP_TOP, 220, 245, 255);
            for (int bx = CUP_LEFT - 1; bx <= CUP_RIGHT + 1; ++bx) {
                if (s_fluid.current_fill > 0.4f) {
                    set_fb_pixel(bx, CUP_BOTTOM, 60, 185, 250);
                } else {
                    set_fb_pixel(bx, CUP_BOTTOM, 120, 165, 190);
                }
            }
            set_fb_pixel(CUP_LEFT - 1, CUP_BOTTOM, 210, 240, 255);
            set_fb_pixel(CUP_RIGHT + 1, CUP_BOTTOM, 210, 240, 255);

            /* Render the final wave wash locally; the shared ripple renderer
             * belongs to the fireworks and campfire scenes. */
            if (water_spill_anim && water_spill_ticks <= 14) {
                float wash_radius = 1.0f + (14 - water_spill_ticks) * 1.45f;
                for (int x = 0; x < CONFIG_LED_MATRIX_WIDTH; ++x) {
                    float distance = fabsf((float) x - 16.0f);
                    if (fabsf(distance - wash_radius) < 1.0f) {
                        add_fb_pixel(x, CUP_BOTTOM, 80, 220, 255);
                        if (CUP_BOTTOM > 0) add_fb_pixel(x, CUP_BOTTOM - 1, 30, 130, 220);
                    }
                }
            }

            if (water_spill_anim && water_spill_ticks <= 0) {
                water_spill_anim = false;
                water_overflow_volume = 0.0f;
                init_fluid_cup();
                clear_particles();
                nextCharacterGame = GAME_8_SHAPES;
                start_game_transition(GAME_2_FIREWORKS_WAND, &currentState,
                                      &transition_wipe_active, &transition_wipe_ticks);
                stateStartTick = now;
                lastInteractionTick = now;
                ESP_LOGI(TAG, "Water spill complete -> GAME_2_FIREWORKS_WAND!");
            }

            if (!water_spill_anim && (now - lastInteractionTick) > pdMS_TO_TICKS(14000)) {
                init_fluid_cup();
                water_overflow_volume = 0.0f;
                nextCharacterGame = GAME_8_SHAPES;
                start_game_transition(GAME_2_FIREWORKS_WAND, &currentState,
                                      &transition_wipe_active, &transition_wipe_ticks);
                stateStartTick = now;
                lastInteractionTick = now;
            }
            break;
        }

        /* ==================================================================
         * GAME 5: CATCH ONE GLOWING FIREFLY
         * ================================================================== */
        case GAME_6_FIREFLY_CATCH: {
            clear_framebuffer();
            firefly_phase += 0.12f;

            /* One clear target, one button press, one bright reward. */
            for (int x = 0; x < CONFIG_LED_MATRIX_WIDTH; ++x) {
                set_fb_pixel(x, 7, 10, 75, 48);
                if ((x % 7) == 1) set_fb_pixel(x, 6, 25, 125, 65);
            }
            set_fb_pixel(2, 1, 30, 80, 180); set_fb_pixel(5, 0, 70, 110, 220);
            set_fb_pixel(23, 1, 60, 90, 180); set_fb_pixel(26, 0, 35, 90, 190);

            int fly_x = 15 + (int) (sinf(firefly_phase * 0.8f) * 7.0f);
            int fly_y = 3 + (int) (sinf(firefly_phase * 1.3f) * 1.0f);

            if (firefly_flash_ticks == 0) {
                set_fb_pixel(fly_x - 2, fly_y, 50, 220, 220);
                set_fb_pixel(fly_x - 1, fly_y - 1, 120, 255, 255);
                set_fb_pixel(fly_x, fly_y - 1, 255, 255, 140);
                set_fb_pixel(fly_x, fly_y, 255, 235, 50);
                set_fb_pixel(fly_x + 1, fly_y, 255, 255, 180);
                set_fb_pixel(fly_x + 2, fly_y, 50, 220, 220);
                set_fb_pixel(fly_x - 1, fly_y + 1, 50, 220, 220);
            }

            if (firefly_flash_ticks > 0) {
                firefly_flash_ticks--;
                for (int star = 0; star < 12; ++star) {
                    int sx = fly_x + (int) (esp_random() % 9) - 4;
                    int sy = fly_y + (int) (esp_random() % 5) - 2;
                    set_fb_pixel(sx, sy, 255, 255, 220);
                }
            } else if (buttonPressed) {
                firefly_caught_count++;
                firefly_flash_ticks = 20;
                spawn_firework_burst((float) fly_x, (float) fly_y, 12);
                if (firefly_caught_count >= 4) {
                    start_game_transition(GAME_7_RED_GREEN_GIRAFFE, &currentState,
                                          &transition_wipe_active, &transition_wipe_ticks);
                    giraffe_awake = false;
                    giraffe_awake_ticks = 0;
                    giraffe_peek_count = 0;
                    stateStartTick = now;
                    lastInteractionTick = now;
                    ESP_LOGI(TAG, "Firefly catches complete -> GAME_7_RED_GREEN_GIRAFFE!");
                }
            }

            if ((now - lastInteractionTick) > pdMS_TO_TICKS(16000)) {
                start_game_transition(GAME_7_RED_GREEN_GIRAFFE, &currentState,
                                      &transition_wipe_active, &transition_wipe_ticks);
                giraffe_awake = false;
                giraffe_awake_ticks = 0;
                giraffe_peek_count = 0;
                stateStartTick = now;
                lastInteractionTick = now;
            }
            break;
        }

        /* ==================================================================
         * GAME 6: SLEEPING GIRAFFE PEEKABOO
         * ================================================================== */
        case GAME_7_RED_GREEN_GIRAFFE: {
            clear_framebuffer();
            if (buttonPressed && !giraffe_awake) {
                giraffe_awake = true;
                giraffe_awake_ticks = 36;
                giraffe_peek_count++;
                spawn_firework_burst(14.0f, 2.0f, 12);
                lastInteractionTick = now;
            }

            if (giraffe_awake) {
                giraffe_awake_ticks--;
                if (giraffe_awake_ticks <= 0) {
                    giraffe_awake = false;
                    if (giraffe_peek_count >= 3) {
                        start_game_transition(GAME_8_SHAPES, &currentState,
                                              &transition_wipe_active, &transition_wipe_ticks);
                        clear_particles();
                        shape_index = 0;
                        shape_pos = 12.0f;
                        shape_velocity = 0.08f;
                        shape_phase = 0.0f;
                        shape_pulse_ticks = 24;
                        shape_finished = false;
                        shape_finish_ticks = 0;
                        stateStartTick = now;
                        lastInteractionTick = now;
                        ESP_LOGI(TAG, "Giraffe peekaboo complete -> GAME_8_SHAPES!");
                    }
                }
            }

            /* A single large sleeping character. Pressing wakes it briefly. */
            for (int x = 0; x < 28; ++x) {
                if ((x % 4) < 2) set_fb_pixel(x, 7, 50, 65, 90);
            }
            uint8_t body_r = giraffe_awake ? 255 : 180;
            uint8_t body_g = giraffe_awake ? 190 : 105;
            uint8_t body_b = giraffe_awake ? 20 : 15;
            for (int x = 8; x <= 12; ++x) {
                set_fb_pixel(x, 5, body_r, body_g, body_b);
                set_fb_pixel(x, 6, body_r / 2, body_g / 2, body_b);
            }
            set_fb_pixel(9, 7, 130, 65, 10); set_fb_pixel(11, 7, 130, 65, 10);
            for (int y = 2; y <= 5; ++y) set_fb_pixel(12, y, body_r, body_g, body_b);
            set_fb_pixel(12, 1, body_r, body_g, body_b);
            set_fb_pixel(13, 1, body_r, body_g, body_b);
            set_fb_pixel(13, 2, body_r, body_g, body_b);
            set_fb_pixel(14, 2, body_r, body_g, body_b);
            set_fb_pixel(14, 1, giraffe_awake ? 255 : 70, giraffe_awake ? 255 : 40, giraffe_awake ? 255 : 20);

            if (giraffe_awake) {
                set_fb_pixel(7, 4, 255, 240, 80); set_fb_pixel(6, 3, 255, 240, 80);
                set_fb_pixel(15, 0, 255, 255, 120); set_fb_pixel(17, 1, 255, 255, 120);
            } else {
                /* Three blue pixels make the sleep cue immediate. */
                set_fb_pixel(17, 0, 90, 160, 255); set_fb_pixel(18, 0, 90, 160, 255);
                set_fb_pixel(17, 1, 90, 160, 255); set_fb_pixel(16, 2, 90, 160, 255);
            }

            if (!giraffe_awake && (now - lastInteractionTick) > pdMS_TO_TICKS(15000)) {
                start_game_transition(GAME_8_SHAPES, &currentState,
                                      &transition_wipe_active, &transition_wipe_ticks);
                clear_particles();
                shape_index = 0;
                shape_pos = 12.0f;
                shape_velocity = 0.08f;
                shape_phase = 0.0f;
                shape_pulse_ticks = 24;
                shape_finished = false;
                shape_finish_ticks = 0;
                stateStartTick = now;
                lastInteractionTick = now;
            }
            break;
        }

        /* ==================================================================
         * GAME 7: BASIC SHAPES & SILHOUETTE RECOGNITION
         * ================================================================== */
        case GAME_8_SHAPES: {
            clear_framebuffer();
            shape_phase += 0.14f;

            /* A calm midnight backdrop keeps the bright silhouette legible. */
            set_fb_pixel(2, 1, 30, 50, 130); set_fb_pixel(5, 6, 25, 60, 140);
            set_fb_pixel(27, 1, 45, 35, 120); set_fb_pixel(24, 6, 35, 55, 130);

            shape_pos += shape_velocity;
            if (shape_pos <= 1.0f) {
                shape_pos = 1.0f;
                shape_velocity = fabsf(shape_velocity);
            } else if (shape_pos >= 25.0f) {
                shape_pos = 25.0f;
                shape_velocity = -fabsf(shape_velocity);
            }

            if (buttonPressed && !shape_finished) {
                shape_index++;
                shape_pulse_ticks = 28;
                if (shape_index >= 4) {
                    shape_index = 4; /* Hold the star long enough to name it. */
                    shape_finished = true;
                    shape_finish_ticks = 52;
                }
                lastInteractionTick = now;
            }

            if (shape_pulse_ticks > 0) {
                shape_pulse_ticks--;
            }
            float pulse = 0.62f + 0.38f * sinf(shape_phase * 1.8f);
            if (shape_pulse_ticks > 0) {
                pulse = 0.72f + 0.28f * sinf(shape_phase * 3.2f);
            }

            uint8_t shape_r = 100;
            uint8_t shape_g = 220;
            uint8_t shape_b = 255;
            switch (shape_index) {
            case 0: shape_r = 60;  shape_g = 225; shape_b = 255; break; /* circle */
            case 1: shape_r = 255; shape_g = 150; shape_b = 45;  break; /* square */
            case 2: shape_r = 255; shape_g = 235; shape_b = 55;  break; /* triangle */
            case 3: shape_r = 255; shape_g = 70;  shape_b = 150; break; /* heart */
            default: shape_r = 180; shape_g = 90;  shape_b = 255; break; /* star */
            }
            draw_shape(shape_index, (int) shape_pos, shape_r, shape_g, shape_b, pulse);

            if (shape_finished) {
                shape_finish_ticks--;
                if (shape_finish_ticks <= 0) {
                    start_game_transition(GAME_9_SPACESHIP, &currentState,
                                          &transition_wipe_active, &transition_wipe_ticks);
                    ship_pos_x = 3.0f;
                    ship_target_x = 3.0f;
                    ship_boosts = 0;
                    ship_boost_ticks = 0;
                    ship_warp_active = false;
                    ship_warp_ticks = 0;
                    init_starfield();
                    stateStartTick = now;
                    lastInteractionTick = now;
                    ESP_LOGI(TAG, "Shape sequence complete -> GAME_9_SPACESHIP!");
                }
            }

            if (!shape_finished && (now - lastInteractionTick) > pdMS_TO_TICKS(18000)) {
                start_game_transition(GAME_9_SPACESHIP, &currentState,
                                      &transition_wipe_active, &transition_wipe_ticks);
                ship_pos_x = 3.0f;
                ship_target_x = 3.0f;
                ship_boosts = 0;
                ship_boost_ticks = 0;
                ship_warp_active = false;
                ship_warp_ticks = 0;
                init_starfield();
                stateStartTick = now;
                lastInteractionTick = now;
            }
            break;
        }

        /* ==================================================================
         * GAME 9: WARP-DRIVE ROCKET SHIP
         * ================================================================== */
        case GAME_9_SPACESHIP: {
            clear_framebuffer();
            ship_bob_phase += 0.12f;

            if (buttonPressed && !ship_warp_active) {
                ship_boosts++;
                ship_boost_ticks = 24;
                ship_target_x = 3.0f + (float) ship_boosts * 4.0f;

                for (int p = 0; p < 8; ++p) {
                    float vx = -0.6f - ((float) (esp_random() % 40) / 100.0f);
                    float vy = ((float) (esp_random() % 21) - 10.0f) * 0.04f;
                    uint8_t pr, pg, pb;
                    hsv_to_rgb(esp_random() % 35, 255, 255, &pr, &pg, &pb);
                    spawn_particle(ship_pos_x, 3.5f, vx, vy, pr, pg, pb, 16, true);
                }

                if (ship_boosts >= 4) {
                    ship_warp_active = true;
                    ship_warp_ticks = 55;
                }
            }

            ship_pos_x += (ship_target_x - ship_pos_x) * 0.18f;

            float speed_mult = ship_warp_active ? 4.5f :
                               (ship_boost_ticks > 0 ? 2.2f : 1.0f);
            for (int i = 0; i < NUM_STARS; ++i) {
                s_stars[i].x -= s_stars[i].speed * speed_mult;
                if (s_stars[i].x < 0.0f) {
                    s_stars[i].x = (float) (CONFIG_LED_MATRIX_WIDTH - 1);
                    s_stars[i].y = (int) (esp_random() % CONFIG_LED_MATRIX_HEIGHT);
                }

                int sx = (int) s_stars[i].x;
                int sy = s_stars[i].y;
                uint8_t sr, sg, sb;
                hsv_to_rgb(s_stars[i].hue, 160, s_stars[i].brightness, &sr, &sg, &sb);

                if (ship_warp_active) {
                    set_fb_pixel(sx, sy, 255, 255, 255);
                    set_fb_pixel(sx + 1, sy, sr, sg, sb);
                    set_fb_pixel(sx + 2, sy, (uint8_t) (sr / 2),
                                 (uint8_t) (sg / 2), (uint8_t) (sb / 2));
                } else {
                    set_fb_pixel(sx, sy, sr, sg, sb);
                }
            }

            if (ship_warp_active) {
                ship_warp_ticks--;
                ship_target_x = 34.0f;
                ship_pos_x += 0.85f;

                if (ship_warp_ticks % 6 == 0) {
                    spawn_particle(31.0f, (float) (esp_random() % 8),
                                   -1.2f, 0.0f, 0, 220, 255, 20, true);
                }

                if (ship_warp_ticks <= 0) {
                    ship_warp_active = false;
                    ship_boosts = 0;
                    ship_pos_x = 3.0f;
                    ship_target_x = 3.0f;
                    spawn_firework_burst(26.0f, 3.5f, 14);
                    spawn_payoff_explosion(false);

                    nextCharacterGame = GAME_1_STRETCHY_GIRAFFE;
                    start_game_transition(GAME_2_FIREWORKS_WAND, &currentState,
                                          &transition_wipe_active, &transition_wipe_ticks);
                    stateStartTick = now;
                    lastInteractionTick = now;
                    ESP_LOGI(TAG, "Spaceship warp complete -> GAME_2_FIREWORKS_WAND!");
                }
            }

            if (ship_boost_ticks > 0) {
                ship_boost_ticks--;
            }

            int ship_draw_y = 1 + (int) (sinf(ship_bob_phase) * 0.8f);
            if (ship_draw_y < 0) ship_draw_y = 0;
            if (ship_draw_y > 3) ship_draw_y = 3;

            float flame_flicker = 0.5f + sinf(flicker_phase * 2.5f) * 0.5f;
            draw_rocket((int) ship_pos_x, ship_draw_y,
                        ship_boost_ticks > 0 || ship_warp_active, flame_flicker);

            if (!ship_warp_active && (now - lastInteractionTick) > pdMS_TO_TICKS(15000)) {
                ship_boosts = 0;
                ship_pos_x = 3.0f;
                ship_target_x = 3.0f;
                nextCharacterGame = GAME_1_STRETCHY_GIRAFFE;
                start_game_transition(GAME_2_FIREWORKS_WAND, &currentState,
                                      &transition_wipe_active, &transition_wipe_ticks);
                stateStartTick = now;
                lastInteractionTick = now;
            }
            break;
        }
        }

        /* Render payoff particles with gravity, floor bounce, and skid. */
        for (int i = 0; i < MAX_PARTICLES; ++i) {
            if (s_particles[i].active) {
                s_particles[i].x += s_particles[i].vx;
                s_particles[i].y += s_particles[i].vy;
                s_particles[i].vy += s_particles[i].gravity;
                s_particles[i].life--;

                if (s_particles[i].y >= (float) (CONFIG_LED_MATRIX_HEIGHT - 1)) {
                    s_particles[i].y = (float) (CONFIG_LED_MATRIX_HEIGHT - 1);
                    s_particles[i].vy = -s_particles[i].vy * 0.35f;
                    s_particles[i].vx *= 0.78f;
                }

                int px = (int) (s_particles[i].x + 0.5f);
                int py = (int) (s_particles[i].y + 0.5f);

                if (s_particles[i].life <= 0 || px < 0 || px >= CONFIG_LED_MATRIX_WIDTH || py < 0 || py >= CONFIG_LED_MATRIX_HEIGHT) {
                    s_particles[i].active = false;
                } else {
                    float fade = (float) s_particles[i].life / (float) s_particles[i].max_life;
                    uint8_t pr = (uint8_t) (s_particles[i].r * fade);
                    uint8_t pg = (uint8_t) (s_particles[i].g * fade);
                    uint8_t pb = (uint8_t) (s_particles[i].b * fade);

                    if (s_particles[i].sparkle && (esp_random() % 3 == 0)) {
                        add_fb_pixel(px, py, 255, 255, 220);
                    } else {
                        add_fb_pixel(px, py, pr, pg, pb);
                    }
                }
            }
        }

        /* Short, muted curtain: enough separation without stealing attention. */
        if (transition_wipe_active) {
            const int wipe_frames = 12;
            transition_wipe_ticks++;
            int sweep_x = ((transition_wipe_ticks *
                            (CONFIG_LED_MATRIX_WIDTH + 4)) / wipe_frames) - 4;

            for (int x = 0; x < CONFIG_LED_MATRIX_WIDTH; ++x) {
                if (x <= sweep_x) {
                    for (int y = 0; y < CONFIG_LED_MATRIX_HEIGHT; ++y) {
                        uint8_t hue = (uint8_t) (155 + ((x + y + transition_wipe_ticks) % 18));
                        uint8_t r, g, b;
                        hsv_to_rgb(hue, 150, sweep_x - x <= 1 ? 95 : 48, &r, &g, &b);
                        set_fb_pixel(x, y, r, g, b);
                    }
                }
            }

            if (transition_wipe_ticks >= wipe_frames) {
                transition_wipe_active = false;
                transition_wipe_ticks = 0;
            }
        }

        /* Flush completed frame to LED matrix */
        ESP_ERROR_CHECK(flush_framebuffer(rf_receiver_signal_recent()));

        /* ~30 FPS frame delay */
        vTaskDelay(pdMS_TO_TICKS(33));
    }
}

/* --------------------------------------------------------------------------
 * Driver Initialization API
 * -------------------------------------------------------------------------- */

esp_err_t led_matrix_init(void)
{
    const led_strip_config_t strip_config = {
        .strip_gpio_num = CONFIG_LED_DATA_GPIO,
        .max_leds = CONFIG_LED_MATRIX_WIDTH * CONFIG_LED_MATRIX_HEIGHT,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags.invert_out = false,
    };
    const led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 64,
        .flags.with_dma = true,
    };

    ESP_RETURN_ON_ERROR(led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip),
                        TAG, "failed to create WS281x RMT driver");
    ESP_LOGI(TAG, "WS281x matrix ready: %dx%d, GRB, GPIO %d",
             CONFIG_LED_MATRIX_WIDTH,
             CONFIG_LED_MATRIX_HEIGHT,
             CONFIG_LED_DATA_GPIO);

    clear_framebuffer();
    return led_strip_clear(s_strip);
}

esp_err_t led_matrix_fill(uint8_t red, uint8_t green, uint8_t blue)
{
    for (uint16_t x = 0; x < CONFIG_LED_MATRIX_WIDTH; ++x) {
        for (uint16_t y = 0; y < CONFIG_LED_MATRIX_HEIGHT; ++y) {
            ESP_RETURN_ON_ERROR(led_matrix_set_xy(x, y, red, green, blue),
                                TAG, "failed to set pixel");
        }
    }
    return led_matrix_refresh();
}

esp_err_t led_matrix_set_xy(uint16_t x, uint16_t y,
                            uint8_t red, uint8_t green, uint8_t blue)
{
    if (s_strip == NULL || x >= CONFIG_LED_MATRIX_WIDTH ||
        y >= CONFIG_LED_MATRIX_HEIGHT) {
        return ESP_ERR_INVALID_ARG;
    }

    return led_strip_set_pixel(s_strip, matrix_index(x, y),
                               scale_brightness(red),
                               scale_brightness(green),
                               scale_brightness(blue));
}

esp_err_t led_matrix_refresh(void)
{
    if (s_strip == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return led_strip_refresh(s_strip);
}

esp_err_t led_matrix_start_demo(void)
{
    if (s_strip == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xTaskCreate(matrix_demo_task, "matrix_demo", 4096, NULL, 4, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "7-game toddler toy suite started");
    return ESP_OK;
}
