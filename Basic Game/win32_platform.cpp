#include <windows.h>
#include <mmsystem.h>
#include <stdint.h>
#include <stdlib.h>
#include <math.h>
#include <stdio.h>

#pragma comment(lib, "winmm.lib")

// -----------------------------------------------------------------------------
// Constants
// -----------------------------------------------------------------------------

static const int LOGICAL_WIDTH = 1280;
static const int LOGICAL_HEIGHT = 720;

static const float PADDLE_WIDTH = 18.0f;
static const float PADDLE_HEIGHT = 115.0f;
static const float PADDLE_ACCEL = 3600.0f;
static const float PADDLE_MAX_SPEED = 760.0f;
static const float PADDLE_FRICTION = 10.0f;

static const float BALL_SIZE = 14.0f;
static const float BALL_SPEED_START = 500.0f;
static const float BALL_SPEED_MAX = 1100.0f;

static const float AI_DEAD_ZONE = 8.0f;
static const float AI_ACCEL_SCALE = 0.90f;

static const float FIXED_DT = 1.0f / 60.0f;

// -----------------------------------------------------------------------------
// Types
// -----------------------------------------------------------------------------

struct Vec2 {
    float x;
    float y;
};

struct RectF {
    float x;
    float y;
    float w;
    float h;
};

struct Paddle {
    RectF rect;
    float vy;
};

struct Ball {
    RectF rect;
    Vec2 vel;
};

enum GameMode {
    GAME_MENU = 0,
    GAME_PLAYING = 1,
    GAME_PAUSED = 2,
};

struct GameState {
    Paddle player;
    Paddle enemy;
    Ball ball;
    int player_score;
    int enemy_score;
    GameMode mode;
};

struct RenderState {
    int width;
    int height;
    void* memory;
    BITMAPINFO bitmap;
};

struct InputState {
    bool up;
    bool down;
    bool start_pressed;
    bool pause_pressed;
    bool reset_pressed;
    bool fullscreen_pressed;
    bool quit_pressed;
};

struct WindowState {
    bool running;
    bool fullscreen;
    WINDOWPLACEMENT placement;
    DWORD style;
};

// -----------------------------------------------------------------------------
// Globals
// -----------------------------------------------------------------------------

static RenderState g_render = {};
static GameState g_game = {};
static WindowState g_window = {true, false, {sizeof(WINDOWPLACEMENT)}, 0};

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------

static float clampf(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static uint32_t rgb(uint8_t r, uint8_t g, uint8_t b) {
    // 32-bit BI_RGB expects 0x00BBGGRR in memory.
    return ((uint32_t)b << 16) | ((uint32_t)g << 8) | (uint32_t)r;
}

static bool intersects(RectF a, RectF b) {
    return (a.x < b.x + b.w &&
            a.x + a.w > b.x &&
            a.y < b.y + b.h &&
            a.y + a.h > b.y);
}

// -----------------------------------------------------------------------------
// Software renderer
// -----------------------------------------------------------------------------

static void clear_screen(uint32_t color) {
    if (!g_render.memory) return;
    uint32_t* pixels = (uint32_t*)g_render.memory;
    int count = g_render.width * g_render.height;
    for (int i = 0; i < count; ++i) pixels[i] = color;
}

static void draw_rect(RectF r, uint32_t color) {
    if (!g_render.memory) return;

    int x0 = (int)r.x;
    int y0 = (int)r.y;
    int x1 = (int)(r.x + r.w);
    int y1 = (int)(r.y + r.h);

    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > g_render.width) x1 = g_render.width;
    if (y1 > g_render.height) y1 = g_render.height;

    for (int y = y0; y < y1; ++y) {
        uint32_t* row = (uint32_t*)g_render.memory + y * g_render.width;
        for (int x = x0; x < x1; ++x) row[x] = color;
    }
}

static void draw_circle(float cx, float cy, float radius, uint32_t color) {
    int x0 = (int)(cx - radius);
    int y0 = (int)(cy - radius);
    int x1 = (int)(cx + radius);
    int y1 = (int)(cy + radius);

    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > g_render.width) x1 = g_render.width;
    if (y1 > g_render.height) y1 = g_render.height;

    float rr = radius * radius;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            float dx = x - cx;
            float dy = y - cy;
            if (dx * dx + dy * dy <= rr) {
                ((uint32_t*)g_render.memory)[y * g_render.width + x] = color;
            }
        }
    }
}

// 5x7 font for numbers and uppercase letters used in UI.
static const uint8_t GLYPH_0[7] = {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E};
static const uint8_t GLYPH_1[7] = {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E};
static const uint8_t GLYPH_2[7] = {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F};
static const uint8_t GLYPH_3[7] = {0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E};
static const uint8_t GLYPH_4[7] = {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02};
static const uint8_t GLYPH_5[7] = {0x1F, 0x10, 0x10, 0x1E, 0x01, 0x01, 0x1E};
static const uint8_t GLYPH_6[7] = {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E};
static const uint8_t GLYPH_7[7] = {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08};
static const uint8_t GLYPH_8[7] = {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E};
static const uint8_t GLYPH_9[7] = {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x1C};

static const uint8_t GLYPH_A[7] = {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11};
static const uint8_t GLYPH_C[7] = {0x0F, 0x10, 0x10, 0x10, 0x10, 0x10, 0x0F};
static const uint8_t GLYPH_D[7] = {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E};
static const uint8_t GLYPH_E[7] = {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F};
static const uint8_t GLYPH_F[7] = {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10};
static const uint8_t GLYPH_G[7] = {0x0F, 0x10, 0x10, 0x17, 0x11, 0x11, 0x0E};
static const uint8_t GLYPH_H[7] = {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11};
static const uint8_t GLYPH_I[7] = {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1F};
static const uint8_t GLYPH_L[7] = {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F};
static const uint8_t GLYPH_M[7] = {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11};
static const uint8_t GLYPH_N[7] = {0x11, 0x19, 0x19, 0x15, 0x13, 0x13, 0x11};
static const uint8_t GLYPH_O[7] = {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E};
static const uint8_t GLYPH_P[7] = {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10};
static const uint8_t GLYPH_Q[7] = {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D};
static const uint8_t GLYPH_R[7] = {0x1E, 0x11, 0x11, 0x1E, 0x12, 0x11, 0x11};
static const uint8_t GLYPH_S[7] = {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E};
static const uint8_t GLYPH_T[7] = {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04};
static const uint8_t GLYPH_U[7] = {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E};
static const uint8_t GLYPH_V[7] = {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04};
static const uint8_t GLYPH_W[7] = {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A};
static const uint8_t GLYPH_Y[7] = {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04};

static const uint8_t GLYPH_COLON[7] = {0x00, 0x04, 0x04, 0x00, 0x04, 0x04, 0x00};
static const uint8_t GLYPH_DASH[7] = {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00};

static const uint8_t* get_glyph(char c) {
    switch (c) {
        case '0': return GLYPH_0;
        case '1': return GLYPH_1;
        case '2': return GLYPH_2;
        case '3': return GLYPH_3;
        case '4': return GLYPH_4;
        case '5': return GLYPH_5;
        case '6': return GLYPH_6;
        case '7': return GLYPH_7;
        case '8': return GLYPH_8;
        case '9': return GLYPH_9;
        case 'A': return GLYPH_A;
        case 'C': return GLYPH_C;
        case 'D': return GLYPH_D;
        case 'E': return GLYPH_E;
        case 'F': return GLYPH_F;
        case 'G': return GLYPH_G;
        case 'H': return GLYPH_H;
        case 'I': return GLYPH_I;
        case 'L': return GLYPH_L;
        case 'M': return GLYPH_M;
        case 'N': return GLYPH_N;
        case 'O': return GLYPH_O;
        case 'P': return GLYPH_P;
        case 'Q': return GLYPH_Q;
        case 'R': return GLYPH_R;
        case 'S': return GLYPH_S;
        case 'T': return GLYPH_T;
        case 'U': return GLYPH_U;
        case 'V': return GLYPH_V;
        case 'W': return GLYPH_W;
        case 'Y': return GLYPH_Y;
        case ':': return GLYPH_COLON;
        case '-': return GLYPH_DASH;
        default: return 0;
    }
}

static void draw_char(char c, int x, int y, int scale, uint32_t color) {
    if (c == ' ') return;
    const uint8_t* glyph = get_glyph(c);
    if (!glyph) return;

    for (int row = 0; row < 7; ++row) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 5; ++col) {
            if (bits & (1 << (4 - col))) {
                RectF px = {(float)(x + col * scale), (float)(y + row * scale), (float)scale, (float)scale};
                draw_rect(px, color);
            }
        }
    }
}

static void draw_text(const char* text, int x, int y, int scale, uint32_t color) {
    int cursor = x;
    while (*text) {
        draw_char(*text, cursor, y, scale, color);
        cursor += 6 * scale;
        ++text;
    }
}

static int text_width(const char* text, int scale) {
    int len = 0;
    while (text[len]) ++len;
    return len * 6 * scale;
}

static void draw_number(int value, int x, int y, int scale, uint32_t color) {
    char buf[16] = {};
    snprintf(buf, sizeof(buf), "%d", value);
    draw_text(buf, x, y, scale, color);
}

// -----------------------------------------------------------------------------
// Game logic
// -----------------------------------------------------------------------------

static void reset_positions(int serve_dir) {
    g_game.player.rect = {30.0f, (LOGICAL_HEIGHT - PADDLE_HEIGHT) * 0.5f, PADDLE_WIDTH, PADDLE_HEIGHT};
    g_game.player.vy = 0.0f;

    g_game.enemy.rect = {LOGICAL_WIDTH - 30.0f - PADDLE_WIDTH, (LOGICAL_HEIGHT - PADDLE_HEIGHT) * 0.5f, PADDLE_WIDTH, PADDLE_HEIGHT};
    g_game.enemy.vy = 0.0f;

    g_game.ball.rect = {(LOGICAL_WIDTH - BALL_SIZE) * 0.5f, (LOGICAL_HEIGHT - BALL_SIZE) * 0.5f, BALL_SIZE, BALL_SIZE};

    float angle = ((float)(rand() % 120) - 60.0f) * 0.0174532925f;
    float sx = cosf(angle);
    float sy = sinf(angle);
    if (sx < 0.20f && sx > -0.20f) sx = (sx < 0.0f) ? -0.20f : 0.20f;

    float dir = (serve_dir >= 0) ? 1.0f : -1.0f;
    g_game.ball.vel.x = fabsf(sx) * BALL_SPEED_START * dir;
    g_game.ball.vel.y = sy * BALL_SPEED_START;
}

static void init_game() {
    g_game.player_score = 0;
    g_game.enemy_score = 0;
    g_game.mode = GAME_MENU;
    reset_positions((rand() % 2) ? 1 : -1);
}

static void apply_paddle_physics(Paddle* paddle, float input_axis, float dt) {
    float accel = input_axis * PADDLE_ACCEL;
    paddle->vy += accel * dt;

    if (input_axis == 0.0f) {
        float damping = 1.0f - PADDLE_FRICTION * dt;
        if (damping < 0.0f) damping = 0.0f;
        paddle->vy *= damping;
    }

    paddle->vy = clampf(paddle->vy, -PADDLE_MAX_SPEED, PADDLE_MAX_SPEED);
    paddle->rect.y += paddle->vy * dt;

    if (paddle->rect.y < 0.0f) {
        paddle->rect.y = 0.0f;
        paddle->vy = 0.0f;
    }

    float max_y = LOGICAL_HEIGHT - paddle->rect.h;
    if (paddle->rect.y > max_y) {
        paddle->rect.y = max_y;
        paddle->vy = 0.0f;
    }
}

static void update_enemy(float dt) {
    float enemy_center = g_game.enemy.rect.y + g_game.enemy.rect.h * 0.5f;
    float target = g_game.ball.rect.y + g_game.ball.rect.h * 0.5f;
    float delta = target - enemy_center;

    float axis = 0.0f;
    if (delta > AI_DEAD_ZONE) axis = 1.0f;
    if (delta < -AI_DEAD_ZONE) axis = -1.0f;

    apply_paddle_physics(&g_game.enemy, axis * AI_ACCEL_SCALE, dt);
}

static void reflect_ball_from_paddle(Paddle* paddle, bool from_left) {
    float paddle_center = paddle->rect.y + paddle->rect.h * 0.5f;
    float ball_center = g_game.ball.rect.y + g_game.ball.rect.h * 0.5f;
    float hit = (ball_center - paddle_center) / (paddle->rect.h * 0.5f);
    hit = clampf(hit, -1.0f, 1.0f);

    float speed = sqrtf(g_game.ball.vel.x * g_game.ball.vel.x + g_game.ball.vel.y * g_game.ball.vel.y);
    speed *= 1.05f;
    if (speed > BALL_SPEED_MAX) speed = BALL_SPEED_MAX;

    float nx = from_left ? 1.0f : -1.0f;
    float ny = hit * 0.75f + paddle->vy / PADDLE_MAX_SPEED * 0.25f;
    float nlen = sqrtf(nx * nx + ny * ny);
    if (nlen <= 0.0f) nlen = 1.0f;
    nx /= nlen;
    ny /= nlen;

    g_game.ball.vel.x = nx * speed;
    g_game.ball.vel.y = ny * speed;
}

static void update_ball(float dt) {
    g_game.ball.rect.x += g_game.ball.vel.x * dt;
    g_game.ball.rect.y += g_game.ball.vel.y * dt;

    if (g_game.ball.rect.y <= 0.0f) {
        g_game.ball.rect.y = 0.0f;
        g_game.ball.vel.y = fabsf(g_game.ball.vel.y);
    }

    float max_y = LOGICAL_HEIGHT - g_game.ball.rect.h;
    if (g_game.ball.rect.y >= max_y) {
        g_game.ball.rect.y = max_y;
        g_game.ball.vel.y = -fabsf(g_game.ball.vel.y);
    }

    if (g_game.ball.vel.x < 0.0f && intersects(g_game.ball.rect, g_game.player.rect)) {
        g_game.ball.rect.x = g_game.player.rect.x + g_game.player.rect.w;
        reflect_ball_from_paddle(&g_game.player, true);
    }

    if (g_game.ball.vel.x > 0.0f && intersects(g_game.ball.rect, g_game.enemy.rect)) {
        g_game.ball.rect.x = g_game.enemy.rect.x - g_game.ball.rect.w;
        reflect_ball_from_paddle(&g_game.enemy, false);
    }

    if (g_game.ball.rect.x + g_game.ball.rect.w < 0.0f) {
        g_game.enemy_score += 1;
        reset_positions(1);
    }

    if (g_game.ball.rect.x > LOGICAL_WIDTH) {
        g_game.player_score += 1;
        reset_positions(-1);
    }
}

static void update_game(const InputState& in, float dt) {
    if (in.quit_pressed) {
        g_window.running = false;
        return;
    }

    if (in.start_pressed && g_game.mode == GAME_MENU) {
        g_game.mode = GAME_PLAYING;
        g_game.player_score = 0;
        g_game.enemy_score = 0;
        reset_positions((rand() % 2) ? 1 : -1);
    }

    if (in.pause_pressed && g_game.mode != GAME_MENU) {
        g_game.mode = (g_game.mode == GAME_PLAYING) ? GAME_PAUSED : GAME_PLAYING;
    }

    if (in.reset_pressed) {
        g_game.player_score = 0;
        g_game.enemy_score = 0;
        g_game.mode = GAME_PLAYING;
        reset_positions((rand() % 2) ? 1 : -1);
    }

    if (g_game.mode != GAME_PLAYING) return;

    float axis = 0.0f;
    if (in.up) axis -= 1.0f;
    if (in.down) axis += 1.0f;

    apply_paddle_physics(&g_game.player, axis, dt);
    update_enemy(dt);
    update_ball(dt);
}

// -----------------------------------------------------------------------------
// Drawing
// -----------------------------------------------------------------------------

static void render_menu() {
    uint32_t title = rgb(0, 230, 170);
    uint32_t text = rgb(220, 230, 235);
    uint32_t dim = rgb(120, 130, 140);

    const char* t1 = "PONG";
    int s1 = 12;
    int x1 = (LOGICAL_WIDTH - text_width(t1, s1)) / 2;
    draw_text(t1, x1, 120, s1, title);

    const char* t2 = "PRESS SPACE TO START";
    int s2 = 4;
    int x2 = (LOGICAL_WIDTH - text_width(t2, s2)) / 2;
    draw_text(t2, x2, 300, s2, text);

    const char* t3 = "W-S MOVE  SPACE PAUSE  R RESET";
    int x3 = (LOGICAL_WIDTH - text_width(t3, 3)) / 2;
    draw_text(t3, x3, 390, 3, dim);

    const char* t4 = "F11 FULLSCREEN  ESC QUIT";
    int x4 = (LOGICAL_WIDTH - text_width(t4, 3)) / 2;
    draw_text(t4, x4, 430, 3, dim);
}

static void render_playfield() {
    uint32_t border = rgb(55, 65, 75);
    uint32_t player = rgb(250, 225, 60);
    uint32_t enemy = rgb(70, 220, 120);
    uint32_t ball = rgb(245, 85, 85);
    uint32_t score = rgb(235, 235, 235);

    for (int y = 0; y < LOGICAL_HEIGHT; y += 24) {
        RectF dash = {(float)(LOGICAL_WIDTH / 2 - 2), (float)y, 4.0f, 12.0f};
        draw_rect(dash, border);
    }

    draw_rect(g_game.player.rect, player);
    draw_rect(g_game.enemy.rect, enemy);

    draw_circle(g_game.ball.rect.x + g_game.ball.rect.w * 0.5f,
                g_game.ball.rect.y + g_game.ball.rect.h * 0.5f,
                g_game.ball.rect.w * 0.5f,
                ball);

    draw_number(g_game.player_score, LOGICAL_WIDTH / 2 - 150, 60, 6, score);
    draw_number(g_game.enemy_score, LOGICAL_WIDTH / 2 + 90, 60, 6, score);
}

static void render_paused_overlay() {
    uint32_t txt = rgb(250, 250, 250);
    uint32_t dim = rgb(170, 170, 170);

    const char* p = "PAUSED";
    int px = (LOGICAL_WIDTH - text_width(p, 7)) / 2;
    draw_text(p, px, 280, 7, txt);

    const char* c = "SPACE TO CONTINUE";
    int cx = (LOGICAL_WIDTH - text_width(c, 3)) / 2;
    draw_text(c, cx, 360, 3, dim);
}

static void render_game() {
    clear_screen(rgb(12, 18, 25));

    if (g_game.mode == GAME_MENU) {
        render_menu();
        return;
    }

    render_playfield();

    if (g_game.mode == GAME_PAUSED) {
        render_paused_overlay();
    }
}

// -----------------------------------------------------------------------------
// Platform
// -----------------------------------------------------------------------------

static void resize_backbuffer(int width, int height) {
    if (width <= 0 || height <= 0) return;

    if (g_render.memory) {
        VirtualFree(g_render.memory, 0, MEM_RELEASE);
        g_render.memory = 0;
    }

    g_render.width = width;
    g_render.height = height;

    int size = width * height * (int)sizeof(uint32_t);
    g_render.memory = VirtualAlloc(0, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

    g_render.bitmap = {};
    g_render.bitmap.bmiHeader.biSize = sizeof(g_render.bitmap.bmiHeader);
    g_render.bitmap.bmiHeader.biWidth = width;
    g_render.bitmap.bmiHeader.biHeight = -height;
    g_render.bitmap.bmiHeader.biPlanes = 1;
    g_render.bitmap.bmiHeader.biBitCount = 32;
    g_render.bitmap.bmiHeader.biCompression = BI_RGB;
}

static void toggle_fullscreen(HWND hwnd) {
    DWORD style = (DWORD)GetWindowLongPtr(hwnd, GWL_STYLE);

    if (!g_window.fullscreen) {
        MONITORINFO mi = {sizeof(mi)};
        if (GetWindowPlacement(hwnd, &g_window.placement) &&
            GetMonitorInfo(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi)) {
            SetWindowLongPtr(hwnd, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
            SetWindowPos(hwnd, HWND_TOP,
                         mi.rcMonitor.left, mi.rcMonitor.top,
                         mi.rcMonitor.right - mi.rcMonitor.left,
                         mi.rcMonitor.bottom - mi.rcMonitor.top,
                         SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
            g_window.fullscreen = true;
        }
    } else {
        SetWindowLongPtr(hwnd, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(hwnd, &g_window.placement);
        SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        g_window.fullscreen = false;
    }
}

LRESULT CALLBACK window_callback(HWND hwnd, UINT msg, WPARAM w_param, LPARAM l_param) {
    switch (msg) {
        case WM_CLOSE:
        case WM_DESTROY: {
            g_window.running = false;
            return 0;
        }

        case WM_ERASEBKGND: {
            return 1;
        }

        case WM_SIZE: {
            RECT rc;
            GetClientRect(hwnd, &rc);
            resize_backbuffer(rc.right - rc.left, rc.bottom - rc.top);
            return 0;
        }

        case WM_SYSKEYDOWN:
        case WM_KEYDOWN: {
            if (w_param == VK_F11) {
                toggle_fullscreen(hwnd);
                return 0;
            }
            return 0;
        }
    }

    return DefWindowProc(hwnd, msg, w_param, l_param);
}

static InputState poll_input() {
    static bool prev_space = false;
    static bool prev_p = false;
    static bool prev_r = false;
    static bool prev_f11 = false;
    static bool prev_escape = false;

    InputState in = {};

    in.up = (GetAsyncKeyState('W') & 0x8000) || (GetAsyncKeyState(VK_UP) & 0x8000);
    in.down = (GetAsyncKeyState('S') & 0x8000) || (GetAsyncKeyState(VK_DOWN) & 0x8000);

    bool now_space = (GetAsyncKeyState(VK_SPACE) & 0x8000) != 0;
    bool now_p = (GetAsyncKeyState('P') & 0x8000) != 0;
    bool now_r = (GetAsyncKeyState('R') & 0x8000) != 0;
    bool now_f11 = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
    bool now_escape = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;

    in.start_pressed = now_space && !prev_space;
    in.pause_pressed = (now_space && !prev_space) || (now_p && !prev_p);
    in.reset_pressed = now_r && !prev_r;
    in.fullscreen_pressed = now_f11 && !prev_f11;
    in.quit_pressed = now_escape && !prev_escape;

    prev_space = now_space;
    prev_p = now_p;
    prev_r = now_r;
    prev_f11 = now_f11;
    prev_escape = now_escape;

    return in;
}

// -----------------------------------------------------------------------------
// Entry
// -----------------------------------------------------------------------------

int WinMain(HINSTANCE h_instance, HINSTANCE, LPSTR, int) {
    srand((unsigned int)GetTickCount());
    timeBeginPeriod(1);

    WNDCLASS wc = {};
    wc.style = CS_OWNDC | CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = window_callback;
    wc.hInstance = h_instance;
    wc.lpszClassName = "PongWindowClass";
    wc.hbrBackground = NULL;

    if (!RegisterClass(&wc)) return 1;

    DWORD style = WS_OVERLAPPEDWINDOW | WS_VISIBLE;
    RECT desired = {0, 0, LOGICAL_WIDTH, LOGICAL_HEIGHT};
    AdjustWindowRect(&desired, style, FALSE);

    HWND hwnd = CreateWindowEx(
        0,
        wc.lpszClassName,
        "Pong - Software Renderer",
        style,
        CW_USEDEFAULT, CW_USEDEFAULT,
        desired.right - desired.left,
        desired.bottom - desired.top,
        0,
        0,
        h_instance,
        0);

    if (!hwnd) {
        timeEndPeriod(1);
        return 1;
    }

    HDC hdc = GetDC(hwnd);
    init_game();

    LARGE_INTEGER perf_freq = {};
    LARGE_INTEGER frame_begin = {};
    QueryPerformanceFrequency(&perf_freq);
    QueryPerformanceCounter(&frame_begin);

    while (g_window.running) {
        MSG msg;
        while (PeekMessage(&msg, 0, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) g_window.running = false;
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        if (!g_window.running) break;

        InputState input = poll_input();

        if (input.fullscreen_pressed) {
            toggle_fullscreen(hwnd);
        }

        update_game(input, FIXED_DT);
        render_game();

        if (g_render.memory) {
            StretchDIBits(hdc,
                          0, 0, g_render.width, g_render.height,
                          0, 0, g_render.width, g_render.height,
                          g_render.memory,
                          &g_render.bitmap,
                          DIB_RGB_COLORS,
                          SRCCOPY);
        }

        LARGE_INTEGER frame_end = {};
        QueryPerformanceCounter(&frame_end);
        double elapsed = (double)(frame_end.QuadPart - frame_begin.QuadPart) / (double)perf_freq.QuadPart;

        if (elapsed < FIXED_DT) {
            DWORD sleep_ms = (DWORD)((FIXED_DT - elapsed) * 1000.0);
            if (sleep_ms > 1) Sleep(sleep_ms - 1);

            do {
                QueryPerformanceCounter(&frame_end);
                elapsed = (double)(frame_end.QuadPart - frame_begin.QuadPart) / (double)perf_freq.QuadPart;
            } while (elapsed < FIXED_DT);
        }

        frame_begin = frame_end;
    }

    if (g_render.memory) VirtualFree(g_render.memory, 0, MEM_RELEASE);
    ReleaseDC(hwnd, hdc);
    timeEndPeriod(1);
    return 0;
}
