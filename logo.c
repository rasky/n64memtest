#include "logo.h"

#include <math.h>
#include <stdbool.h>
#include <stddef.h>

#define LOGO_STICK_DEADZONE         10
#define LOGO_MANUAL_TIMEOUT_MS      5000U
#define LOGO_MAX_DT_MS              100U
#define LOGO_MANUAL_YAW_SPEED       2.2f
#define LOGO_MANUAL_PITCH_SPEED     1.8f
#define LOGO_AUTO_YAW_SPEED         1.25f
#define LOGO_AUTO_PITCH_SPEED       0.65f
#define LOGO_AUTO_PITCH_CENTER      0.34f
#define LOGO_AUTO_PITCH_AMPLITUDE   0.20f
#define LOGO_PITCH_MIN              -0.80f
#define LOGO_PITCH_MAX              0.95f

typedef struct {
    float x;
    float y;
    float z;
} logo_vertex_t;

typedef struct {
    uint8_t a;
    uint8_t b;
} logo_edge_t;

typedef struct {
    int x;
    int y;
    bool visible;
} logo_point2d_t;

static const logo_vertex_t k_logo_vertices[] = {
    {-74.892f, -75.103f, 37.428f}, /* 0 */
    {-74.892f, -75.103f, 18.696f}, /* 1 */
    {-37.427f, -75.103f, 18.695f}, /* 2 */
    {-37.427f, -75.103f, 74.892f}, /* 3 */
    {-74.892f, -75.103f, 74.893f}, /* 4 */
    {-37.427f, -75.103f, 37.428f}, /* 5 */
    {-37.427f, 75.103f, -18.694f}, /* 6 */
    {-74.892f, 75.103f, -74.891f}, /* 7 */
    {-74.892f, 75.103f, -18.694f}, /* 8 */
    {-37.427f, 75.103f, -74.891f}, /* 9 */
    {-74.892f, -75.103f, -74.891f}, /* 10 */
    {-37.427f, -75.103f, -74.891f}, /* 11 */
    {-74.892f, -75.103f, -37.426f}, /* 12 */
    {-18.695f, -75.103f, -37.427f}, /* 13 */
    {-37.427f, -75.103f, -37.427f}, /* 14 */
    {-18.695f, -75.103f, -74.892f}, /* 15 */
    {18.695f, 75.103f, -74.892f}, /* 16 */
    {18.695f, 75.103f, -37.427f}, /* 17 */
    {74.892f, 75.103f, -74.893f}, /* 18 */
    {74.892f, 75.103f, -37.428f}, /* 19 */
    {74.892f, -75.103f, -37.428f}, /* 20 */
    {37.427f, -75.103f, -74.892f}, /* 21 */
    {74.892f, -75.103f, -74.893f}, /* 22 */
    {74.892f, -75.103f, -18.696f}, /* 23 */
    {37.427f, -75.103f, -18.695f}, /* 24 */
    {37.427f, -75.103f, -37.428f}, /* 25 */
    {37.427f, 75.103f, 18.694f}, /* 26 */
    {74.892f, 75.103f, 74.891f}, /* 27 */
    {74.892f, 75.103f, 18.694f}, /* 28 */
    {37.427f, 75.103f, 74.891f}, /* 29 */
    {74.892f, -75.103f, 37.426f}, /* 30 */
    {74.892f, -75.103f, 74.891f}, /* 31 */
    {37.427f, -75.103f, 74.891f}, /* 32 */
    {18.695f, -75.103f, 37.427f}, /* 33 */
    {37.427f, -75.103f, 37.427f}, /* 34 */
    {18.695f, -75.103f, 74.892f}, /* 35 */
    {-18.695f, 75.103f, 74.892f}, /* 36 */
    {-18.695f, 75.103f, 37.427f}, /* 37 */
    {-74.892f, 75.103f, 74.893f}, /* 38 */
    {-74.892f, 75.103f, 37.428f}, /* 39 */
    {-37.427f, -37.551f, 37.428f}, /* 40 */
    {-37.427f, 37.551f, -37.427f}, /* 41 */
    {-37.427f, -37.551f, -74.891f}, /* 42 */
    {-37.427f, 37.551f, 37.428f}, /* 43 */
    {-37.427f, 37.551f, 74.892f}, /* 44 */
    {-74.892f, -37.551f, 37.428f}, /* 45 */
    {-74.892f, 37.551f, -37.426f}, /* 46 */
    {-74.892f, -37.551f, -74.891f}, /* 47 */
    {-74.892f, -37.551f, 74.893f}, /* 48 */
    {37.427f, 37.551f, -37.428f}, /* 49 */
    {37.427f, 37.551f, -74.892f}, /* 50 */
    {74.892f, -37.551f, -37.428f}, /* 51 */
    {74.892f, -37.551f, -74.893f}, /* 52 */
    {74.892f, 37.551f, 37.426f}, /* 53 */
    {74.892f, -37.551f, 74.891f}, /* 54 */
    {37.427f, 37.551f, 37.427f}, /* 55 */
    {37.427f, -37.551f, -37.428f}, /* 56 */
    {37.427f, -37.551f, 74.891f}, /* 57 */
    {-37.427f, -37.551f, -37.427f}, /* 58 */
    {37.427f, -37.551f, 37.427f}, /* 59 */
};

static const uint8_t k_logo_faces[][3] = {
    {1, 0, 5}, {1, 5, 2}, {5, 0, 4}, {5, 4, 3}, {9, 6, 8}, {9, 8, 7},
    {14, 11, 10}, {14, 10, 12}, {15, 11, 14}, {15, 14, 13}, {19, 17, 16}, {19, 16, 18},
    {25, 20, 22}, {25, 22, 21}, {23, 20, 25}, {23, 25, 24}, {29, 26, 28}, {29, 28, 27},
    {34, 32, 31}, {34, 31, 30}, {35, 32, 34}, {35, 34, 33}, {39, 37, 36}, {39, 36, 38},
    {2, 5, 40}, {41, 2, 40}, {41, 40, 6}, {41, 6, 9}, {14, 41, 9}, {14, 9, 42},
    {14, 42, 11}, {43, 5, 3}, {43, 3, 44}, {1, 45, 0}, {46, 45, 1}, {46, 7, 8},
    {46, 8, 45}, {12, 7, 46}, {12, 47, 7}, {12, 10, 47}, {4, 0, 45}, {4, 45, 48},
    {48, 45, 39}, {48, 39, 38}, {49, 25, 21}, {49, 21, 50}, {22, 20, 51}, {22, 51, 52},
    {52, 51, 19}, {52, 19, 18}, {23, 51, 20}, {53, 51, 23}, {53, 28, 51}, {53, 27, 28},
    {30, 27, 53}, {30, 54, 27}, {30, 31, 54}, {24, 25, 56}, {55, 26, 29}, {55, 56, 26},
    {55, 24, 56}, {34, 55, 29}, {34, 29, 57}, {34, 57, 32}, {2, 41, 46}, {2, 46, 1},
    {17, 58, 42}, {17, 42, 16}, {24, 55, 53}, {24, 53, 23}, {37, 59, 57}, {37, 57, 36},
    {6, 40, 45}, {6, 45, 8}, {41, 14, 12}, {41, 12, 46}, {13, 14, 58}, {49, 13, 58},
    {49, 58, 17}, {49, 17, 19}, {25, 49, 19}, {25, 19, 51}, {25, 51, 20}, {10, 11, 42},
    {10, 42, 47}, {47, 42, 9}, {47, 9, 7}, {15, 42, 11}, {50, 42, 15}, {50, 16, 42},
    {50, 18, 16}, {21, 18, 50}, {21, 52, 18}, {21, 22, 52}, {13, 49, 50}, {13, 50, 15},
    {26, 56, 51}, {26, 51, 28}, {55, 34, 30}, {55, 30, 53}, {33, 34, 59}, {43, 33, 59},
    {43, 59, 37}, {43, 37, 39}, {31, 32, 57}, {31, 57, 54}, {54, 57, 29}, {54, 29, 27},
    {35, 57, 32}, {44, 57, 35}, {44, 36, 57}, {44, 38, 36}, {3, 38, 44}, {3, 48, 38},
    {3, 4, 48}, {33, 43, 44}, {33, 44, 35}, {5, 45, 0}, {5, 39, 45}, {5, 43, 39},
};

/* Coarse contour/spine edges used as stable guides. */
static const logo_edge_t k_logo_key_edges[] = {
    {0, 1}, {0, 4}, {0, 5}, {1, 2}, {1, 5}, {2, 5}, {3, 4}, {3, 5},
    {3, 38}, {4, 5}, {5, 39}, {6, 8}, {6, 9}, {7, 8}, {7, 9}, {7, 12},
    {8, 9}, {9, 14}, {10, 11}, {10, 12}, {10, 14}, {11, 14}, {11, 15}, {12, 14},
    {13, 14}, {13, 15}, {14, 15}, {16, 17}, {16, 18}, {16, 19}, {17, 19}, {18, 19},
    {18, 21}, {19, 25}, {20, 22}, {20, 23}, {20, 25}, {21, 22}, {21, 25}, {22, 25},
    {23, 24}, {23, 25}, {24, 25}, {26, 28}, {26, 29}, {27, 28}, {27, 29}, {27, 30},
    {28, 29}, {29, 34}, {30, 31}, {30, 34}, {31, 32}, {31, 34}, {32, 34}, {32, 35},
    {33, 34}, {33, 35}, {34, 35}, {36, 37}, {36, 38}, {36, 39}, {37, 39}, {38, 39},
};

#define LOGO_VERT_COUNT      ((int)(sizeof(k_logo_vertices) / sizeof(k_logo_vertices[0])))
#define LOGO_FACE_COUNT      ((int)(sizeof(k_logo_faces) / sizeof(k_logo_faces[0])))
#define LOGO_KEY_EDGE_COUNT  ((int)(sizeof(k_logo_key_edges) / sizeof(k_logo_key_edges[0])))
#define LOGO_MAX_EDGES       (LOGO_FACE_COUNT * 3)

static logo_edge_t g_logo_edges[LOGO_MAX_EDGES];
static int16_t g_edge_face_a[LOGO_MAX_EDGES];
static int16_t g_edge_face_b[LOGO_MAX_EDGES];
static bool g_edge_is_key[LOGO_MAX_EDGES];
static int g_logo_edge_count = 0;
static bool g_topology_ready = false;
static bool g_manual_control = false;
static uint32_t g_last_input_ms = 0;
static uint32_t g_last_frame_ms = 0;
static float g_manual_yaw = 0.0f;
static float g_manual_pitch = LOGO_AUTO_PITCH_CENTER;
static bool g_auto_initialized = false;
static float g_auto_yaw = 0.0f;
static float g_auto_pitch_phase = 0.0f;

static inline int abs_int(int v)
{
    return (v < 0) ? -v : v;
}

static float clampf(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static float nearest_phase(float phase, float reference)
{
    const float two_pi = 6.283185307f;
    float k = roundf((reference - phase) / two_pi);
    return phase + k * two_pi;
}

static float phase_from_pitch(float pitch, float reference_phase)
{
    float s = (pitch - LOGO_AUTO_PITCH_CENTER) / LOGO_AUTO_PITCH_AMPLITUDE;
    float base;
    float alt;
    float c0;
    float c1;

    s = clampf(s, -1.0f, 1.0f);
    base = asinf(s);
    alt = 3.141592654f - base;
    c0 = nearest_phase(base, reference_phase);
    c1 = nearest_phase(alt, reference_phase);
    return (fabsf(c1 - reference_phase) < fabsf(c0 - reference_phase)) ? c1 : c0;
}

static void normalize_edge(uint8_t *a, uint8_t *b)
{
    if (*a > *b) {
        uint8_t t = *a;
        *a = *b;
        *b = t;
    }
}

static int find_runtime_edge(uint8_t a, uint8_t b)
{
    int i;
    for (i = 0; i < g_logo_edge_count; i++) {
        if (g_logo_edges[i].a == a && g_logo_edges[i].b == b) return i;
    }
    return -1;
}

static void add_face_edge(uint8_t a, uint8_t b, int face_index)
{
    int edge_index;
    normalize_edge(&a, &b);
    if (a == b) return;

    edge_index = find_runtime_edge(a, b);
    if (edge_index < 0) {
        if (g_logo_edge_count >= LOGO_MAX_EDGES) return;
        edge_index = g_logo_edge_count++;
        g_logo_edges[edge_index].a = a;
        g_logo_edges[edge_index].b = b;
        g_edge_face_a[edge_index] = -1;
        g_edge_face_b[edge_index] = -1;
        g_edge_is_key[edge_index] = false;
    }

    if (g_edge_face_a[edge_index] < 0) g_edge_face_a[edge_index] = (int16_t)face_index;
    else if (g_edge_face_b[edge_index] < 0 && g_edge_face_a[edge_index] != face_index) g_edge_face_b[edge_index] = (int16_t)face_index;
}

static void init_logo_topology(void)
{
    int i;

    g_logo_edge_count = 0;
    for (i = 0; i < LOGO_FACE_COUNT; i++) {
        const uint8_t *tri = k_logo_faces[i];
        add_face_edge(tri[0], tri[1], i);
        add_face_edge(tri[1], tri[2], i);
        add_face_edge(tri[2], tri[0], i);
    }

    for (i = 0; i < LOGO_KEY_EDGE_COUNT; i++) {
        uint8_t a = k_logo_key_edges[i].a;
        uint8_t b = k_logo_key_edges[i].b;
        int edge_index;
        normalize_edge(&a, &b);
        edge_index = find_runtime_edge(a, b);
        if (edge_index >= 0) g_edge_is_key[edge_index] = true;
    }

    g_topology_ready = true;
}

static void logo_compute_angles(uint32_t ticks_ms, int analog_x, int analog_y, float *yaw, float *pitch)
{
    uint32_t dt_ms = g_last_frame_ms ? (ticks_ms - g_last_frame_ms) : 16U;
    float dt;
    float norm_x;
    float norm_y;
    bool has_input;

    if (dt_ms > LOGO_MAX_DT_MS) dt_ms = LOGO_MAX_DT_MS;
    g_last_frame_ms = ticks_ms;

    if (!g_auto_initialized) {
        float t = (float)ticks_ms * 0.001f;
        g_auto_yaw = t * LOGO_AUTO_YAW_SPEED;
        g_auto_pitch_phase = t * LOGO_AUTO_PITCH_SPEED;
        g_auto_initialized = true;
    }

    norm_x = (float)analog_x / 90.0f;
    norm_y = (float)analog_y / 90.0f;
    has_input = (abs_int(analog_x) > LOGO_STICK_DEADZONE) || (abs_int(analog_y) > LOGO_STICK_DEADZONE);
    dt = (float)dt_ms * 0.001f;

    if (has_input) {
        if (!g_manual_control) {
            g_manual_control = true;
            g_manual_yaw = g_auto_yaw;
            g_manual_pitch = LOGO_AUTO_PITCH_CENTER + LOGO_AUTO_PITCH_AMPLITUDE * sinf(g_auto_pitch_phase);
        }
        g_manual_yaw += norm_x * LOGO_MANUAL_YAW_SPEED * dt;
        g_manual_pitch += norm_y * LOGO_MANUAL_PITCH_SPEED * dt;
        g_manual_pitch = clampf(g_manual_pitch, LOGO_PITCH_MIN, LOGO_PITCH_MAX);
        g_last_input_ms = ticks_ms;
        *yaw = g_manual_yaw;
        *pitch = g_manual_pitch;
        return;
    }

    if (g_manual_control) {
        if ((ticks_ms - g_last_input_ms) < LOGO_MANUAL_TIMEOUT_MS) {
            *yaw = g_manual_yaw;
            *pitch = g_manual_pitch;
            return;
        }

        g_manual_control = false;
        g_auto_yaw = g_manual_yaw;
        g_auto_pitch_phase = phase_from_pitch(g_manual_pitch, g_auto_pitch_phase);
        *yaw = g_auto_yaw;
        *pitch = g_manual_pitch;
        return;
    }

    g_auto_yaw += LOGO_AUTO_YAW_SPEED * dt;
    g_auto_pitch_phase += LOGO_AUTO_PITCH_SPEED * dt;
    *yaw = g_auto_yaw;
    *pitch = LOGO_AUTO_PITCH_CENTER + LOGO_AUTO_PITCH_AMPLITUDE * sinf(g_auto_pitch_phase);
}

static bool project_logo_vertex(const logo_vertex_t *v, float yaw, float pitch,
                                int center_x, int center_y, logo_point2d_t *out)
{
    const float sin_y = sinf(yaw);
    const float cos_y = cosf(yaw);
    const float sin_x = sinf(pitch);
    const float cos_x = cosf(pitch);
    const float camera_z = 3.8f;
    const float focal = 58.0f;
    const float scale = 1.0f / 75.0f;

    float x = v->x * scale;
    float y = v->y * scale;
    float z = v->z * scale;

    float x_yaw = x * cos_y + z * sin_y;
    float z_yaw = -x * sin_y + z * cos_y;

    float y_pitch = y * cos_x - z_yaw * sin_x;
    float z_pitch = y * sin_x + z_yaw * cos_x;

    float denom = camera_z - z_pitch;
    if (denom < 0.20f) {
        out->visible = false;
        return false;
    }

    {
        float persp = focal / denom;
        out->x = center_x + (int)lroundf(x_yaw * persp);
        out->y = center_y - (int)lroundf(y_pitch * persp);
        out->visible = true;
    }
    return true;
}

void logo_draw(surface_t *disp, int center_x, int center_y, uint32_t color,
               uint32_t ticks_ms, int analog_x, int analog_y)
{
    logo_point2d_t projected[LOGO_VERT_COUNT];
    bool face_front[LOGO_FACE_COUNT];
    bool face_valid[LOGO_FACE_COUNT];
    float yaw;
    float pitch;
    int i;

    if (!g_topology_ready) init_logo_topology();
    logo_compute_angles(ticks_ms, analog_x, analog_y, &yaw, &pitch);

    for (i = 0; i < LOGO_VERT_COUNT; i++) {
        project_logo_vertex(&k_logo_vertices[i], yaw, pitch, center_x, center_y, &projected[i]);
    }

    for (i = 0; i < LOGO_FACE_COUNT; i++) {
        const uint8_t *tri = k_logo_faces[i];
        const logo_point2d_t *p0 = &projected[tri[0]];
        const logo_point2d_t *p1 = &projected[tri[1]];
        const logo_point2d_t *p2 = &projected[tri[2]];
        if (!p0->visible || !p1->visible || !p2->visible) {
            face_valid[i] = false;
            face_front[i] = false;
            continue;
        }
        {
            int area2 = (p1->x - p0->x) * (p2->y - p0->y) - (p1->y - p0->y) * (p2->x - p0->x);
            face_valid[i] = true;
            face_front[i] = area2 < 0;
        }
    }

    for (i = 0; i < g_logo_edge_count; i++) {
        const logo_edge_t e = g_logo_edges[i];
        const logo_point2d_t *a = &projected[e.a];
        const logo_point2d_t *b = &projected[e.b];
        int f0 = g_edge_face_a[i];
        int f1 = g_edge_face_b[i];
        bool silhouette = false;
        if (!a->visible || !b->visible) continue;

        if (f0 >= 0 && f1 >= 0) {
            if (face_valid[f0] && face_valid[f1]) silhouette = (face_front[f0] != face_front[f1]);
            else silhouette = (face_valid[f0] != face_valid[f1]);
        } else if ((f0 >= 0 && face_valid[f0]) || (f1 >= 0 && face_valid[f1])) {
            silhouette = true;
        }

        if (!silhouette && !g_edge_is_key[i]) continue;
        graphics_draw_line(disp, a->x, a->y, b->x, b->y, color);
    }
}
