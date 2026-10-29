//
// 光影 Lights And Shadows —— KernelLoader (tefkernel) 模组，PC 端
//
// 和 PE 端同一套思路：屏幕后处理。
//   世界 + UI 画完 → 在 GraphicsDevice.Present 之前：
//     · 把当前帧读成场景纹理
//     · 缩到 1/3 按蓝色通道提取阴影（树/地面蓝少 → 阴影强）
//     · 阴影沿太阳方向逐层放大叠加成条纹，「光 − 阴影」漏出丁达尔光柱
//     · 以太阳为中心的宽光晕 + 光柱 additive 叠回，再乘黄/白色温
//
// PC 走桌面 OpenGL（FNA 后端）：
//   · 不依赖 GL/D3D：GetBackBufferData 读回场景，CPU 处理后用
//     Texture2D + SpriteBatch 画回，Windows/Linux 的 XNA/FNA 都能用
//   · 用 patchlib 挂 Main.DrawInterface 的 prefix，在世界画完、UI 开始前绘制
//   · 太阳/时间等状态仍由 Main.DrawInterface_36_Cursor 的 postfix 写快照
//

#include "mod-api/mod_core.h"
#include "mod-api/mod_logger.h"
#include "tefkernel/patchlib/type.h"
#include "tefkernel/patchlib/method.h"
#include "tefkernel/patchlib/field.h"
#include "tefkernel/patchlib/struct/array.h"
#include "tefkernel/tefstd/vector.h"
#include "tefkernel/terraria/main.h"
#include "tefkernel/terraria/texture2d.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void (*mod_logger_write)(mod_log_level_t level, const char* tag, const char* fmt, ...) = NULL;

#define LOG_INFO(...)  do { if (mod_logger_write) mod_logger_write(MOD_LOG_LEVEL_INFO,  "LightsAndShadows", __VA_ARGS__); } while (0)
#define LOG_ERROR(...) do { if (mod_logger_write) mod_logger_write(MOD_LOG_LEVEL_ERROR, "LightsAndShadows", __VA_ARGS__); } while (0)

#define MOD_VERSION "2.6.3-light"

/* ============================================================================
 * 配置
 *   首选：TEFManager 设置界面 → <私有目录>/config.json（改完重启游戏生效）
 *   调试：<私有目录>/ls_settings.txt，覆盖同名字段（不用重装包）
 *
 * config.json（TEFManager 生成）：
 *   {"schemaVersion":1,"values":{"enabled":true,"quality":"balanced",
 *    "ray_samples":16,"shadow_div":3,"glow":60,"rays":55,
 *    "shadow_strength":135,"light_color":"warm"}}
 *
 * ls_settings.txt：
 *   glow=0.60   光晕强度（0 = 关掉）
 *   rays=0.55   光柱强度（0 = 关掉，只剩光晕）
 *   shadow_strength=1.35  阴影强度 0.3..2.5（1.0 = 基准）
 *   light=warm  光色：warm=黄光，white=白光
 *   taps=16     光柱采样数 4..30（越大越细腻、越慢）
 *   shadow=3    阴影/光柱降采样倍数 2..6（越小越清晰、越慢）
 *   sun=-1      调试：强制太阳时刻 0..1（<0 = 用游戏时间）
 * ========================================================================== */

enum { Q_PERF = 0, Q_BALANCED = 1, Q_VISUAL = 2, Q_CUSTOM = 3 };

static int   g_enabled    = 1;
static int   g_quality    = Q_BALANCED;
static int   g_taps       = 16;     /* 光柱采样数，决定用哪份着色器 */
static int   g_shadow_div = 3;      /* 阴影图 = 画面 1/N */
static float g_cfg_glow   = 0.60f;
static float g_cfg_rays   = 0.55f;
static float g_cfg_shadow = 1.35f;  /* 阴影强度 0.3..2.5，1.0 = 基准 */
static int   g_light_color = 0;     /* 0 = 黄光（默认），1 = 白光 */
static float g_cfg_sun    = -1.0f;
static char  g_private_dir[1024];

static const char *quality_name(int q) {
    switch (q) {
        case Q_PERF:   return "性能";
        case Q_VISUAL: return "视觉";
        case Q_CUSTOM: return "自定义";
        default:       return "平衡";
    }
}

static float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* 档位 → 具体参数；自定义档直接用界面里填的四个值 */
static void apply_preset(int q, int taps, int div, float glow, float rays) {
    switch (q) {
        case Q_PERF:   taps = 8;  div = 5; glow = 0.60f; rays = 0.55f; break;
        case Q_VISUAL: taps = 30; div = 2; glow = 0.65f; rays = 0.70f; break;
        case Q_CUSTOM: break;
        default:       taps = 16; div = 3; glow = 0.60f; rays = 0.55f; break;
    }
    if (taps < 4) taps = 4;
    if (taps > 30) taps = 30;
    if (div < 2) div = 2;
    if (div > 6) div = 6;
    g_taps       = taps;
    g_shadow_div = div;
    g_cfg_glow   = clampf(glow, 0.0f, 1.5f);
    g_cfg_rays   = clampf(rays, 0.0f, 1.5f);
}

/* —— 极简 JSON 取值：config.json 结构固定，不做通用解析 —— */
static char *read_whole_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n < 0 || n > 65536) { fclose(f); return NULL; }
    rewind(f);
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, f);
    buf[got] = 0;
    fclose(f);
    return buf;
}

static const char *json_value(const char *obj, const char *key) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(obj, pat);
    if (!p) return NULL;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
    if (*p != ':') return NULL;
    ++p;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
    return p;
}

static int json_int(const char *o, const char *k, int d) {
    const char *v = json_value(o, k);
    if (!v) return d;
    char *end = NULL;
    long n = strtol(v, &end, 10);
    if (end == v) return d;
    return (int)n;
}

static int json_bool(const char *o, const char *k, int d) {
    const char *v = json_value(o, k);
    if (!v) return d;
    if (v[0] == 't' || v[0] == 'T' || v[0] == '1') return 1;
    if (v[0] == 'f' || v[0] == 'F' || v[0] == '0') return 0;
    return d;
}

static void json_str(const char *o, const char *k, char *out, size_t cap, const char *d) {
    const char *v = json_value(o, k);
    snprintf(out, cap, "%s", d);
    if (!v || *v != '"') return;
    ++v;
    size_t i = 0;
    while (*v && *v != '"' && i + 1 < cap) out[i++] = *v++;
    out[i] = 0;
}

static void read_config_json(void) {
    char path[1200];
    int taps = 16, div = 3, glow = 60, rays = 55;
    g_enabled = 1;
    g_quality = Q_BALANCED;

    if (g_private_dir[0]) {
        snprintf(path, sizeof(path), "%s/config.json", g_private_dir);
        char *buf = read_whole_file(path);
        if (buf) {
            const char *vals = strstr(buf, "\"values\"");
            const char *o = vals ? vals : buf;

            g_enabled = json_bool(o, "enabled", 1) ? 1 : 0;
            taps = json_int(o, "ray_samples", 16);
            div  = json_int(o, "shadow_div", 3);
            glow = json_int(o, "glow", 60);
            rays = json_int(o, "rays", 55);

            g_cfg_shadow = clampf((float)json_int(o, "shadow_strength", 135) / 100.0f,
                                  0.3f, 2.5f);
            char lc[16];
            json_str(o, "light_color", lc, sizeof(lc), "warm");
            g_light_color = (!strcmp(lc, "white") || !strcmp(lc, "cool")) ? 1 : 0;

            char q[16];
            json_str(o, "quality", q, sizeof(q), "balanced");
            if (!strcmp(q, "perf") || !strcmp(q, "performance")) g_quality = Q_PERF;
            else if (!strcmp(q, "visual")) g_quality = Q_VISUAL;
            else if (!strcmp(q, "custom")) g_quality = Q_CUSTOM;
            else g_quality = Q_BALANCED;

            free(buf);
        }
    }
    apply_preset(g_quality, taps, div, (float)glow / 100.0f, (float)rays / 100.0f);
}

/* 调试覆盖：ls_settings.txt（可选，写什么覆盖什么） */
static void read_settings_txt(void) {
    if (!g_private_dir[0]) return;
    char path[1200];
    snprintf(path, sizeof(path), "%s/ls_settings.txt", g_private_dir);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *hash = strchr(line, '#'); if (hash) *hash = 0;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = line;
        while (*key == ' ' || *key == '\t') ++key;
        char *end = key + strlen(key);
        while (end > key && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) *--end = 0;
        char *val = eq + 1;
        while (*val == ' ' || *val == '\t') ++val;
        float v = (float)atof(val);
        if (!strcmp(key, "glow")) g_cfg_glow = clampf(v, 0.0f, 1.5f);
        else if (!strcmp(key, "rays")) g_cfg_rays = clampf(v, 0.0f, 1.5f);
        else if (!strcmp(key, "taps")) g_taps = (int)clampf(v, 4.0f, 30.0f);
        else if (!strcmp(key, "shadow")) g_shadow_div = (int)clampf(v, 2.0f, 6.0f);
        else if (!strcmp(key, "shadow_strength") || !strcmp(key, "shadowstr"))
            g_cfg_shadow = clampf(v, 0.3f, 2.5f);
        else if (!strcmp(key, "light") || !strcmp(key, "light_color") ||
                 !strcmp(key, "color"))
            g_light_color = (!strncmp(val, "white", 5) || !strncmp(val, "1", 1)) ? 1 : 0;
        else if (!strcmp(key, "sun")) g_cfg_sun = v;
    }
    fclose(f);
}

/* ============================================================================
 * 一、游戏状态读取
 * ========================================================================== */

typedef struct { float x, y; } v2_t;

static patch_handle_t g_main_type;
static int g_frames;

static patch_handle_t main_instance(void) {
    static patch_handle_t inst;
    if (inst != PATCH_NULL) return inst;
    patch_handle_t f = patchlib_type_get_field(g_main_type, "instance");
    if (!f) return PATCH_NULL;
    patch_handle_t v = PATCH_NULL;
    patchlib_field_get_value(f, NULL, &v);
    inst = v;
    return inst;
}

/* 桌面没有 Android 的 field_get_pointer，统一用 field_get_value 拷贝。 */
static bool main_field_value(const char *name, void *out, size_t size) {
    patch_handle_t f = patchlib_type_get_field(g_main_type, name);
    if (!f) return false;
    memset(out, 0, size);
    patchlib_field_get_value(f, NULL, out);
    return true;
}

/* 0 参 getter：用 patchlib_method_invoke_args，避免桌面/手机 ABI 差异。 */
static bool main_getter_value(const char *name, void *out, size_t size) {
    char buf[80];
    snprintf(buf, sizeof(buf), "get_%s", name);
    patch_handle_t g = patchlib_type_get_method_by_param_count(g_main_type, buf, 0);
    if (!g || !patchlib_is_valid(g)) return false;
    memset(out, 0, size);
    patch_handle_t inst = patchlib_method_is_static(g) ? PATCH_NULL : main_instance();
    if (!patchlib_method_invoke_args(g, inst, out, NULL)) return false;
    return true;
}

static int main_int(const char *name, int fallback) {
    int v;
    if (main_field_value(name, &v, sizeof(v))) return v;
    if (main_getter_value(name, &v, sizeof(v))) return v;
    return fallback;
}

static bool main_bool(const char *name, bool fallback) {
    unsigned char v = 0;
    if (main_field_value(name, &v, sizeof(v))) return v != 0;
    if (main_getter_value(name, &v, sizeof(v))) return v != 0;
    return fallback;
}

static double main_double(const char *name, double fallback) {
    double v;
    if (main_field_value(name, &v, sizeof(v))) return v;
    if (main_getter_value(name, &v, sizeof(v))) return v;
    return fallback;
}

static bool main_vec2(const char *name, float *x, float *y) {
    v2_t v;
    if (main_field_value(name, &v, sizeof(v)) ||
        main_getter_value(name, &v, sizeof(v))) {
        *x = v.x;
        *y = v.y;
        return true;
    }
    return false;
}

static bool player_center(float *cx, float *cy) {
    patch_handle_t player_type = patchlib_type_get_type("Terraria", "Player");
    if (!player_type) return false;
    patch_handle_t player = PATCH_NULL;
    if (!main_getter_value("LocalPlayer", &player, sizeof(player))) return false;
    if (player == PATCH_NULL) return false;

    patch_handle_t f_pos = patchlib_type_get_field(player_type, "position");
    if (!f_pos) return false;
    v2_t pos;
    memset(&pos, 0, sizeof(pos));
    patchlib_field_get_value(f_pos, player, &pos);
    int w = 20, h = 42;
    patch_handle_t f_w = patchlib_type_get_field(player_type, "width");
    patch_handle_t f_h = patchlib_type_get_field(player_type, "height");
    if (f_w) patchlib_field_get_value(f_w, player, &w);
    if (f_h) patchlib_field_get_value(f_h, player, &h);
    if (!isfinite(pos.x) || !isfinite(pos.y)) return false;
    *cx = pos.x + w * 0.5f;
    *cy = pos.y + h * 0.5f;
    return true;
}

/* ============================================================================
 * 二、快照（游戏线程写、渲染线程读）
 * ========================================================================== */

typedef struct {
    int   enabled;
    int   virtW, virtH;
    float sunU, sunV;            /* 太阳在 GL UV 里的位置（y 向上） */
    int   sunExact;              /* 1 = 取自游戏真实位置 */
    float shadowM;               /* 阴影提取阈值：蓝通道低于它算阴影 */
    float shadowK;               /* 阴影强度（夜里关掉，0 = 不画条纹） */
    float warmR, warmG, warmB;   /* 光晕颜色 */
    float gradeR, gradeG, gradeB;/* 黄光/白光的整屏色温 */
    float glowA;                 /* 光晕强度 */
    float topR, topG, topB;      /* 天空顶部色（乘法） */
    float botR, botG, botB;      /* 地平线色（乘法） */
    float vignette;              /* 暗角强度 0..1 */
} light_snap_t;

static light_snap_t g_snap[2];
static volatile int g_snap_head = -1;

static light_snap_t *snap_back(void) { return &g_snap[(g_snap_head + 1) % 2]; }
static void snap_commit(void) { g_snap_head = (g_snap_head + 1) % 2; }

/* ============================================================================
 * 三、光照模型（自己的公式与色板）
 * ========================================================================== */

static void compute_light(light_snap_t *s) {
    int sw = s->virtW, sh = s->virtH;

    bool day = main_bool("dayTime", true);
    double time = main_double("time", 0.0);
    int moon = main_int("moonPhase", 0);

    double span = day ? 54000.0 : 32400.0;
    float t01 = (float)(time / span);
    if (t01 < 0.0f) t01 = 0.0f;
    if (t01 > 1.0f) t01 = 1.0f;
    if (g_cfg_sun >= 0.0f) t01 = g_cfg_sun;   /* 测试用：强制太阳时刻 */

    float ang = 3.14159265f * t01;   /* 光强/月相都按这个时刻算 */

    /* 太阳位置：照原版 GetSunPos 的算法推（t 取到正午=屏幕中线，上下随镜头高度偏移）。
     * 不读 LastCelestialBodyPosition —— 那个值在 PE 上会被背景视差临时改写，读出来是跳的。 */
    double ws = main_double("worldSurface", 0.0);
    float camY = 0.0f, camX = 0.0f;
    if (!main_vec2("screenPosition", &camX, &camY)) camY = 0.0f;
    float skyShift = 0.0f;
    if (ws > 10.0) skyShift = (-camY) / (float)(ws * 16.0 - 600.0) * 200.0f;
    float halfDay = day ? 27000.0f : 16200.0f;
    float k = 1.0f - (float)(time / halfDay);   /* 1=日出/月出  0=正中  -1=日落 */
    float px = (1.0f - k) * (float)sw * 0.5f - 100.0f * k;
    float py = skyShift + k * k * 250.0f + 180.0f;
    s->sunU = px / (float)sw;
    s->sunV = 1.0f - py / (float)sh;
    s->sunExact = 1;

    if ((g_frames % 600) == 1) {
        LOG_INFO("太阳诊断：k=%.2f 时间=%.0f 白天=%d 相机y=%.0f 世界表面=%.1f 屏幕=%dx%d",
                 k, time, day ? 1 : 0, camY, ws, sw, sh);
    }
    /* 阴影：白天按蓝色通道（天空很蓝，b≈0.75；树/地面 b≈0.2）取阈值；
     * 夜里整屏都暗，蓝通道区分不出天空，直接把条纹关掉 */
    s->shadowM = day ? 0.50f : 0.02f;
    s->shadowK = day ? 1.0f : 0.0f;

    /* 亮度曲线：sin^0.8 —— 正午平台宽、日出日落更快暗下去 */
    float noon = sinf(ang);
    float lift = powf(noon < 0.0f ? 0.0f : noon, 0.8f);
    /* 月相：0=满月最亮 … 4=新月最暗 */
    float moonK = 0.55f + 0.45f * cosf(6.2831853f * (float)moon / 8.0f);

    const float DAY_GAIN = 0.75f, NIGHT_GAIN = 0.55f;
    float intensity = day ? (0.35f + DAY_GAIN * lift)
                          : (0.30f + NIGHT_GAIN * lift) * moonK;

    /* 光色：黄光走暖黄（正午偏金黄），白光固定为略偏冷的中性白 */
    if (day) {
        if (t01 < 0.5f) {
            float u = t01 * 2.0f;
            s->warmR = 0.95f + 0.05f * u;
            s->warmG = 0.64f + 0.23f * u;
            s->warmB = 0.32f + 0.14f * u;
        } else {
            float u = (t01 - 0.5f) * 2.0f;
            s->warmR = 1.00f;
            s->warmG = 0.87f - 0.28f * u;
            s->warmB = 0.46f - 0.26f * u;
        }
    } else {
        /* 黄光模式下的月光也保留一点暖琥珀色，不偏蓝 */
        float w = (1.0f - moonK) * 0.5f;
        s->warmR = 0.78f + 0.08f * w;
        s->warmG = 0.70f + 0.05f * w;
        s->warmB = 0.50f - 0.04f * w;
    }
    if (g_light_color == 1) {
        s->warmR = 0.96f;
        s->warmG = 0.98f;
        s->warmB = 1.00f;
        /* 白光只靠光晕太不明显，再给整屏一个很淡的冷色，和暖色拉开差距 */
        s->gradeR = 0.985f;
        s->gradeG = 0.995f;
        s->gradeB = 1.03f;
    } else {
        /* 整屏色温只压一点点蓝，避免像老旧电视 */
        s->gradeR = day ? 1.035f : 1.02f;
        s->gradeG = day ? 0.985f : 0.995f;
        s->gradeB = day ? 0.84f : 0.93f;
    }

    s->glowA = intensity * g_cfg_glow;
    if (s->glowA > 1.2f) s->glowA = 1.2f;

    /* 整屏乘法色罩：天空垂直渐变 + 暗角。
     * 白天顶部略偏冷、地平线随日落渐暖；夜里顶部深蓝、地平线稍亮。 */
    if (day) {
        float d = 1.0f - lift;               /* 越接近日落 d 越大 */
        s->topR = 0.90f;
        s->topG = 0.95f;
        s->topB = 1.02f;
        s->botR = 1.00f;
        s->botG = 0.98f - 0.16f * d;
        s->botB = 0.90f - 0.32f * d;
        s->vignette = 0.18f;
    } else {
        float k = 0.90f + 0.10f * moonK;
        s->topR = 0.30f * k;
        s->topG = 0.42f * k;
        s->topB = 0.74f * k;
        s->botR = 0.46f * k;
        s->botG = 0.53f * k;
        s->botB = 0.86f * k;
        s->vignette = 0.30f;
    }
}


/* ============================================================================
 * 五、PC：XNA API 屏幕后处理
 *
 * 不再依赖 OpenGL/D3D 后端：
 *   1. Main.DrawInterface 的 prefix（世界画完、UI 还没开始）用
 *      GraphicsDevice.GetBackBufferData 把场景读回 CPU
 *   2. CPU 在低分辨率下提阴影/走光柱，再按完整分辨率做色温和叠光
 *   3. 用 Texture2D + SpriteBatch 把结果画回屏幕，UI 随后正常覆盖
 *
 * PC 性能足够，CPU 版本避免 GL/D3D 上下文和着色器兼容问题。
 * ========================================================================== */

static int g_pc_state;              /* 0=未初始化 1=可用 -1=失败 */
static patch_handle_t g_pc_gd;
static patch_handle_t g_pc_color_type, g_pc_uint8_type;
static patch_handle_t g_pc_gd_type, g_pc_tex_type, g_pc_sb_type;
static patch_handle_t g_pc_m_backbuffer, g_pc_m_setdata, g_pc_m_draw;
static patch_handle_t g_pc_m_sb_begin, g_pc_m_sb_end, g_pc_m_viewport;
static patch_handle_t g_pc_sb_field, g_pc_gch_type, g_pc_m_gch_alloc;

static patch_handle_t g_pc_tex, g_pc_tex_array;
static void *g_pc_tex_pin, *g_pc_arr_pin;
static int g_pc_w, g_pc_h;          /* CPU 缓冲尺寸 */
static int g_pc_tex_w, g_pc_tex_h;  /* 纹理尺寸（缩放后要重建） */
static unsigned char *g_pc_scene, *g_pc_out;
static float *g_pc_blue, *g_pc_shadow, *g_pc_light;

static patch_handle_t pc_generic_one(patch_handle_t method, patch_handle_t type) {
    if (!patchlib_is_valid(method) || !patchlib_is_valid(type)) return PATCH_NULL;
    tefstd_vector_t types;
    tefstd_vector_init(&types, sizeof(patch_handle_t));
    tefstd_vector_push_back(&types, &type);
    patch_handle_t out = patchlib_method_make_generic_instance(method, &types);
    tefstd_vector_destroy(&types);
    return out;
}

static void *pc_pin(patch_handle_t obj) {
    if (!patchlib_is_valid(g_pc_m_gch_alloc) || !patchlib_is_valid(obj)) return NULL;
    void *pin = NULL;
    void *args[1] = { &obj };
    if (!patchlib_method_invoke_args(g_pc_m_gch_alloc, PATCH_NULL, &pin, args)) return NULL;
    return pin;
}

static bool pc_init_xna(void) {
    if (g_pc_state != 0) return g_pc_state > 0;
    g_pc_state = -1;

    g_pc_gd = terraria_main_get_graphics_device();
    if (!patchlib_is_valid(g_pc_gd)) {
        LOG_ERROR("PC: 取不到 GraphicsDevice");
        return false;
    }

    g_pc_gd_type = patchlib_type_get_type("Microsoft.Xna.Framework.Graphics", "GraphicsDevice");
    g_pc_tex_type = patchlib_type_get_type("Microsoft.Xna.Framework.Graphics", "Texture2D");
    g_pc_sb_type = patchlib_type_get_type("Microsoft.Xna.Framework.Graphics", "SpriteBatch");
    g_pc_color_type = patchlib_type_get_type("Microsoft.Xna.Framework", "Color");
    g_pc_uint8_type = patchlib_get_basic_type(PATCH_UINT8);
    if (!patchlib_is_valid(g_pc_color_type))
        g_pc_color_type = patchlib_type_get_type("Microsoft.Xna.Framework.Graphics", "Color");

    patch_handle_t m_bb = patchlib_type_get_method_by_param_count(
            g_pc_gd_type, "GetBackBufferData", 1);
    patch_handle_t m_sd = patchlib_type_get_method_by_param_count(
            g_pc_tex_type, "SetData", 1);
    g_pc_m_backbuffer = pc_generic_one(m_bb, g_pc_color_type);
    g_pc_m_setdata = pc_generic_one(m_sd, g_pc_uint8_type);
    g_pc_m_sb_begin = patchlib_type_get_method_by_param_count(g_pc_sb_type, "Begin", 0);
    g_pc_m_sb_end = patchlib_type_get_method_by_param_count(g_pc_sb_type, "End", 0);
    g_pc_m_viewport = patchlib_type_get_method_by_param_count(g_pc_gd_type, "get_Viewport", 0);

    patch_handle_t t_rect = patchlib_type_get_type("Microsoft.Xna.Framework", "Rectangle");
    patch_handle_t draw_types[3] = { g_pc_tex_type, t_rect, g_pc_color_type };
    g_pc_m_draw = patchlib_type_get_method_by_param_types(g_pc_sb_type, "Draw", 3, draw_types);

    g_pc_sb_field = patchlib_type_get_field(g_main_type, "spriteBatch");

    patch_handle_t t_gch = patchlib_type_get_type("System.Runtime.InteropServices", "GCHandle");
    g_pc_m_gch_alloc = patchlib_type_get_method_by_param_count(t_gch, "Alloc", 1);

    if (!patchlib_is_valid(g_pc_m_backbuffer) || !patchlib_is_valid(g_pc_m_sb_begin) ||
        !patchlib_is_valid(g_pc_m_sb_end) || !patchlib_is_valid(g_pc_m_draw)) {
        LOG_ERROR("PC: XNA 绘制 API 不完整（bb=%d begin=%d end=%d draw=%d）",
                  patchlib_is_valid(g_pc_m_backbuffer),
                  patchlib_is_valid(g_pc_m_sb_begin), patchlib_is_valid(g_pc_m_sb_end),
                  patchlib_is_valid(g_pc_m_draw));
        return false;
    }

    g_pc_state = 1;
    LOG_INFO("PC: XNA 屏幕后处理就绪（GetBackBufferData + Texture2D + SpriteBatch）");
    return true;
}

static bool pc_resize(int W, int H) {
    if (W <= 0 || H <= 0) return false;
    if (g_pc_scene && g_pc_w == W && g_pc_h == H) return true;

    unsigned char *scene = (unsigned char *) realloc(g_pc_scene, (size_t) W * H * 4);
    unsigned char *out = (unsigned char *) realloc(g_pc_out, (size_t) W * H * 4);
    if (!scene || !out) {
        free(scene); free(out);
        return false;
    }
    g_pc_scene = scene;
    g_pc_out = out;

    int sw = W / g_shadow_div, sh = H / g_shadow_div;
    if (sw < 1) sw = 1;
    if (sh < 1) sh = 1;
    float *blue = (float *) realloc(g_pc_blue, (size_t) sw * sh * sizeof(float));
    float *shadow = (float *) realloc(g_pc_shadow, (size_t) sw * sh * sizeof(float));
    float *light = (float *) realloc(g_pc_light, (size_t) sw * sh * sizeof(float) * 3);
    if (!blue || !shadow || !light) {
        free(blue); free(shadow); free(light);
        return false;
    }
    g_pc_blue = blue;
    g_pc_shadow = shadow;
    g_pc_light = light;
    g_pc_w = W;
    g_pc_h = H;
    return true;
}

static bool pc_capture(int W, int H) {
    patch_handle_t arr = patchlib_array_create((size_t) W * H, g_pc_color_type);
    if (!patchlib_is_valid(arr)) return false;
    void *args[1] = { &arr };
    bool ok = patchlib_method_invoke_args(g_pc_m_backbuffer, g_pc_gd, NULL, args);
    if (ok) patchlib_array_copy_to_c(g_pc_scene, arr, (size_t) W * H);
    patchlib_free(arr);
    return ok;
}

static inline float pc_clampf(float v, float a, float b) {
    return v < a ? a : (v > b ? b : v);
}

static inline float pc_smooth(float a, float b, float x) {
    if (a == b) return x < a ? 0.0f : 1.0f;
    float t = pc_clampf((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

static float pc_shadow_at(const float *mask, int sw, int sh, float x, float y) {
    x -= 0.5f;
    y -= 0.5f;
    int x0 = (int) floorf(x), y0 = (int) floorf(y);
    float fx = x - x0, fy = y - y0;
    int x1 = x0 + 1, y1 = y0 + 1;
    if (x0 < 0) x0 = 0; if (x0 >= sw) x0 = sw - 1;
    if (x1 < 0) x1 = 0; if (x1 >= sw) x1 = sw - 1;
    if (y0 < 0) y0 = 0; if (y0 >= sh) y0 = sh - 1;
    if (y1 < 0) y1 = 0; if (y1 >= sh) y1 = sh - 1;
    float a = mask[y0 * sw + x0], b = mask[y0 * sw + x1];
    float c = mask[y1 * sw + x0], d = mask[y1 * sw + x1];
    return (a + (b - a) * fx) + ((c + (d - c) * fx) - (a + (b - a) * fx)) * fy;
}

static void pc_compute(const light_snap_t *s, int W, int H) {
    int sw = W / g_shadow_div, sh = H / g_shadow_div;
    if (sw < 1) sw = 1;
    if (sh < 1) sh = 1;
    int taps = g_taps;
    if (taps < 4) taps = 4;
    if (taps > 30) taps = 30;

    /* 低分辨率蓝通道 → 阴影 */
    for (int y = 0; y < sh; ++y) {
        int sy = y * H / sh;
        for (int x = 0; x < sw; ++x) {
            int sx = x * W / sw;
            g_pc_blue[y * sw + x] = g_pc_scene[((size_t) sy * W + sx) * 4 + 2] / 255.0f;
        }
    }
    float m = s->shadowM;
    for (int y = 0; y < sh; ++y) {
        for (int x = 0; x < sw; ++x) {
            float b = 0.0f;
            for (int dy = -1; dy <= 1; ++dy) {
                int yy = y + dy; if (yy < 0) yy = 0; if (yy >= sh) yy = sh - 1;
                for (int dx = -1; dx <= 1; ++dx) {
                    int xx = x + dx; if (xx < 0) xx = 0; if (xx >= sw) xx = sw - 1;
                    b += g_pc_blue[yy * sw + xx];
                }
            }
            b /= 9.0f;
            float v = powf(pc_clampf(1.0f - b, 0.0f, 1.0f), 0.75f);
            v *= 1.0f - pc_smooth(m * 0.72f, m, b);
            g_pc_shadow[y * sw + x] = s->shadowK > 0.001f ? v : 0.0f;
        }
    }

    /* 光柱：沿太阳方向缩放采样阴影 */
    float sunY = 1.0f - s->sunV;
    float aspect = (float) sw / (float) sh;
    float zoom = 3.2f / (float) taps;
    float shade_k = 0.09f * s->shadowK * g_cfg_rays * g_cfg_shadow;
    for (int y = 0; y < sh; ++y) {
        float uvy = (y + 0.5f) / (float) sh;
        for (int x = 0; x < sw; ++x) {
            float uvx = (x + 0.5f) / (float) sw;
            float dx = (uvx - s->sunU) * aspect;
            float dy = uvy - sunY;
            float r = sqrtf(dx * dx + dy * dy);
            float fall = 1.0f - r / 1.15f;
            if (fall < 0.0f) fall = 0.0f;
            fall *= fall;

            float shade = 0.0f;
            if (s->shadowK > 0.001f) {
                float sum = 0.0f;
                for (int i = 0; i < taps; ++i) {
                    float k = 1.0f + (float) i * zoom;
                    float px = s->sunU + (uvx - s->sunU) / k;
                    float py = sunY + (uvy - sunY) / k;
                    sum += pc_shadow_at(g_pc_shadow, sw, sh, px * sw, py * sh) *
                           (1.0f - (float) i / 30.0f);
                }
                shade = pc_clampf(sum * shade_k, 0.0f, 1.0f);
            }
            float lit = 1.0f - 0.96f * shade;
            float v = fall * s->glowA * lit;
            size_t o = ((size_t) y * sw + x) * 3;
            g_pc_light[o + 0] = s->warmR * v;
            g_pc_light[o + 1] = s->warmG * v;
            g_pc_light[o + 2] = s->warmB * v;
        }
    }

    /* 全分辨率：场景色温 + 低分辨率光照上采样 */
    for (int y = 0; y < H; ++y) {
        float ly = ((float) y + 0.5f) * sh / H - 0.5f;
        int y0 = (int) floorf(ly), y1 = y0 + 1;
        float fy = ly - y0;
        if (y0 < 0) y0 = 0; if (y0 >= sh) y0 = sh - 1;
        if (y1 < 0) y1 = 0; if (y1 >= sh) y1 = sh - 1;

        for (int x = 0; x < W; ++x) {
            float lx = ((float) x + 0.5f) * sw / W - 0.5f;
            int x0 = (int) floorf(lx), x1 = x0 + 1;
            float fx = lx - x0;
            if (x0 < 0) x0 = 0; if (x0 >= sw) x0 = sw - 1;
            if (x1 < 0) x1 = 0; if (x1 >= sw) x1 = sw - 1;

            size_t i00 = ((size_t) y0 * sw + x0) * 3;
            size_t i10 = ((size_t) y0 * sw + x1) * 3;
            size_t i01 = ((size_t) y1 * sw + x0) * 3;
            size_t i11 = ((size_t) y1 * sw + x1) * 3;
            float a0 = g_pc_light[i00] + (g_pc_light[i10] - g_pc_light[i00]) * fx;
            float a1 = g_pc_light[i01] + (g_pc_light[i11] - g_pc_light[i01]) * fx;
            float lr = a0 + (a1 - a0) * fy;
            a0 = g_pc_light[i00 + 1] + (g_pc_light[i10 + 1] - g_pc_light[i00 + 1]) * fx;
            a1 = g_pc_light[i01 + 1] + (g_pc_light[i11 + 1] - g_pc_light[i01 + 1]) * fx;
            float lg = a0 + (a1 - a0) * fy;
            a0 = g_pc_light[i00 + 2] + (g_pc_light[i10 + 2] - g_pc_light[i00 + 2]) * fx;
            a1 = g_pc_light[i01 + 2] + (g_pc_light[i11 + 2] - g_pc_light[i01 + 2]) * fx;
            float lb = a0 + (a1 - a0) * fy;

            size_t p = ((size_t) y * W + x) * 4;
            float sr = g_pc_scene[p + 0] / 255.0f;
            float sg = g_pc_scene[p + 1] / 255.0f;
            float sb = g_pc_scene[p + 2] / 255.0f;
            g_pc_out[p + 0] = (unsigned char) (pc_clampf(sr * s->gradeR + lr, 0, 1) * 255.0f + 0.5f);
            g_pc_out[p + 1] = (unsigned char) (pc_clampf(sg * s->gradeG + lg, 0, 1) * 255.0f + 0.5f);
            g_pc_out[p + 2] = (unsigned char) (pc_clampf(sb * s->gradeB + lb, 0, 1) * 255.0f + 0.5f);
            g_pc_out[p + 3] = 255;
        }
    }
}

static bool pc_ensure_texture(int W, int H) {
    if (patchlib_is_valid(g_pc_tex) && g_pc_tex_w == W && g_pc_tex_h == H &&
        patchlib_is_valid(g_pc_tex_array))
        return true;

    g_pc_tex = terraria_texture2d_create(W, H, TEXTURE_FORMAT_RGBA32,
                                         g_pc_out, (size_t) W * H * 4);
    if (!patchlib_is_valid(g_pc_tex)) {
        LOG_ERROR("PC: Texture2D 创建失败");
        return false;
    }
    g_pc_tex_pin = pc_pin(g_pc_tex);

    g_pc_tex_array = patchlib_array_create((size_t) W * H * 4, g_pc_uint8_type);
    if (!patchlib_is_valid(g_pc_tex_array)) {
        LOG_ERROR("PC: 纹理字节数组创建失败");
        return false;
    }
    g_pc_arr_pin = pc_pin(g_pc_tex_array);
    g_pc_tex_w = W;
    g_pc_tex_h = H;
    return true;
}

static void pc_draw_texture(int W, int H) {
    patch_handle_t sb = PATCH_NULL;
    if (patchlib_is_valid(g_pc_sb_field))
        patchlib_field_get_value(g_pc_sb_field, NULL, &sb);
    if (!patchlib_is_valid(sb)) return;

    if (!patchlib_is_valid(g_pc_m_setdata)) {
        static int once;
        if (!once) { once = 1; LOG_ERROR("PC: Texture2D.SetData 不可用"); }
        return;
    }
    if (!patchlib_array_copy_from_c(g_pc_tex_array, g_pc_out, (size_t) W * H * 4)) return;
    void *sd_args[1] = { &g_pc_tex_array };
    patchlib_method_invoke_args(g_pc_m_setdata, g_pc_tex, NULL, sd_args);

    patchlib_method_invoke_args(g_pc_m_sb_begin, sb, NULL, NULL);

    struct { int x, y, w, h; } rect = { 0, 0, W, H };
    uint32_t white = 0xFFFFFFFFu;
    void *draw_args[3] = { &g_pc_tex, &rect, &white };
    patchlib_method_invoke_args(g_pc_m_draw, sb, NULL, draw_args);

    patchlib_method_invoke_args(g_pc_m_sb_end, sb, NULL, NULL);
}

static void pc_render(const light_snap_t *s) {
    if (!s->enabled || s->virtW <= 0 || s->virtH <= 0) return;
    if (!pc_init_xna()) return;
    int W = s->virtW, H = s->virtH;
    if (patchlib_is_valid(g_pc_m_viewport)) {
        struct { int x, y, w, h; } vp = { 0, 0, 0, 0 };
        if (patchlib_method_invoke_args(g_pc_m_viewport, g_pc_gd, &vp, NULL) &&
            vp.w > 0 && vp.h > 0) {
            W = vp.w;
            H = vp.h;
        }
    }
    if (!pc_resize(W, H)) return;
    if (!pc_capture(W, H)) {
        static int once;
        if (!once) { once = 1; LOG_ERROR("PC: GetBackBufferData 失败"); }
        return;
    }
    pc_compute(s, W, H);
    if (!pc_ensure_texture(W, H)) return;
    pc_draw_texture(W, H);

    static int once;
    if (!once) {
        once = 1;
        LOG_INFO("PC: 首次绘制 %dx%d（阴影 1/%d，采样 %d）", W, H, g_shadow_div, g_taps);
    }
}

/* ============================================================================
 * 六、PC：Main.DrawInterface prefix（世界画完、UI 之前）
 * ========================================================================== */

static patch_hook_id_t g_hook_interface = PATCH_HOOK_INVALID_ID;
static patch_hook_id_t g_hook_bars = PATCH_HOOK_INVALID_ID;
static volatile int g_in_hook;

static bool on_interface(patch_handle_t instance, void **args,
                         const patch_method_signature_t *sig, void *result) {
    (void)instance; (void)args; (void)sig; (void)result;
    if (!g_in_hook) {
        g_in_hook = 1;
        int i = g_snap_head;
        if (i >= 0) pc_render(&g_snap[i]);
        g_in_hook = 0;
    }
    return false;
}

static bool install_interface_hook(void) {
    patch_handle_t iface =
        patchlib_type_get_method_by_param_count(g_main_type, "DrawInterface", 1);
    if (patchlib_is_valid(iface)) {
        g_hook_interface = patchlib_install_prepost_hook(iface, on_interface, NULL);
        if (g_hook_interface != PATCH_HOOK_INVALID_ID) {
            LOG_INFO("PC: 已挂 Main.DrawInterface（世界后、UI 前）");
            return true;
        }
    }

    /* 兜底：在资源条/Buff 栏之前画，至少 Buff 栏和后面的 UI 不会被盖 */
    patch_handle_t bars = patchlib_type_get_method_by_param_count(g_main_type, "GUIBarsDraw", 0);
    if (patchlib_is_valid(bars)) {
        g_hook_bars = patchlib_install_prepost_hook(bars, on_interface, NULL);
        if (g_hook_bars != PATCH_HOOK_INVALID_ID) {
            LOG_INFO("PC: DrawInterface 不可用，改挂 GUIBarsDraw");
            return true;
        }
    }
    LOG_ERROR("PC: 找不到 DrawInterface/GUIBarsDraw");
    return false;
}

static void uninstall_interface_hook(void) {
    if (g_hook_interface != PATCH_HOOK_INVALID_ID) {
        patchlib_uninstall_hook(g_hook_interface);
        g_hook_interface = PATCH_HOOK_INVALID_ID;
    }
    if (g_hook_bars != PATCH_HOOK_INVALID_ID) {
        patchlib_uninstall_hook(g_hook_bars);
        g_hook_bars = PATCH_HOOK_INVALID_ID;
    }
}

/* ============================================================================
 * 六、每帧：读状态 → 写快照（游戏线程，不画任何东西）
 * ========================================================================== */

static patch_hook_id_t g_hook_draw = PATCH_HOOK_INVALID_ID;
static bool g_logged_splash;

static void on_draw(patch_handle_t instance, void **args, void *result,
                    const patch_method_signature_t *sig) {
    (void)instance; (void)args; (void)result; (void)sig;

    light_snap_t *s = snap_back();
    memset(s, 0, sizeof(*s));

    if (!g_enabled) { snap_commit(); return; }

    int sw = main_int("screenWidth", 0), sh = main_int("screenHeight", 0);
    if (sw <= 0 || sh <= 0) { snap_commit(); return; }
    s->virtW = sw;
    s->virtH = sh;

    if (main_bool("showSplash", true)) {
        if (!g_logged_splash) { g_logged_splash = true; LOG_INFO("启动画面中，暂不绘制"); }
        snap_commit(); return;
    }
    if (main_bool("gameMenu", true)) { snap_commit(); return; }

    compute_light(s);

    /* 地下：不画光照（按深度判，树/屋檐在地表以上不会被误判） */
    {
        double worldSurface = main_double("worldSurface", -1.0);
        float pcx = 0, pcy = 0;
        if (worldSurface > 10.0 && player_center(&pcx, &pcy)) {
            float tiles = pcy / 16.0f;
            if (tiles > (float)worldSurface + 6.0f) {
                if ((g_frames % 600) == 1) LOG_INFO("已在地表以下较深位置，不画光照");
                snap_commit(); return;
            }
        }
    }

    s->enabled = 1;
    g_frames++;
    snap_commit();

    if ((g_frames % 600) == 1) {
        LOG_INFO("档位=%s 采样=%d 阴影=1/%d 太阳uvx1000=(%d,%d) 光强x100=%d 光柱x100=%d 阴影x100=%d",
                 quality_name(g_quality), g_taps, g_shadow_div,
                 (int)(s->sunU * 1000), (int)(s->sunV * 1000),
                 (int)(s->glowA * 100), (int)(g_cfg_rays * 100),
                 (int)(s->shadowK * 100));
    }
}

/* ============================================================================
 * 七、入口
 * ========================================================================== */

static const kernel_mod_info_t g_info = {
    .pkg_id = "lzup.lightsandshadows",
    .version_code = 15,
    .api_version = 1,
    .version = MOD_VERSION,
};

static void init_mod(kernel_mod_handle_t *handle) {
    if (handle && handle->private_dir)
        snprintf(g_private_dir, sizeof(g_private_dir), "%s", handle->private_dir);

    g_main_type = patchlib_type_get_type("Terraria", "Main");
    if (!g_main_type) { LOG_ERROR("找不到 Terraria.Main"); return; }

    patch_handle_t draw_method =
        patchlib_type_get_method_by_param_count(g_main_type, "DrawInterface_36_Cursor", 0);
    if (!draw_method || !patchlib_is_valid(draw_method)) {
        LOG_ERROR("找不到 Main.DrawInterface_36_Cursor");
        return;
    }
    g_hook_draw = patchlib_install_prepost_hook(draw_method, NULL, on_draw);
    if (g_hook_draw == PATCH_HOOK_INVALID_ID) { LOG_ERROR("快照钩子安装失败"); return; }

    if (!install_interface_hook()) LOG_ERROR("屏幕后处理绘制钩子安装失败");

    read_config_json();
    read_settings_txt();
    LOG_INFO("配置：%s 档（采样=%d 阴影=1/%d 强度=%.2fx 光色=%s 光晕=%.2f 光柱=%.2f）%s",
             quality_name(g_quality), g_taps, g_shadow_div, g_cfg_shadow,
             g_light_color ? "白光" : "黄光", g_cfg_glow, g_cfg_rays,
             g_enabled ? "" : " — 已关闭，不绘制");
    LOG_INFO("初始化完成 v%s（快照 hook=%d 界面 hook=%d bars=%d）", MOD_VERSION,
             (int)g_hook_draw, (int)g_hook_interface, (int)g_hook_bars);
}

static void cleanup_mod(kernel_mod_handle_t *handle) {
    (void)handle;
    uninstall_interface_hook();
    if (g_hook_draw != PATCH_HOOK_INVALID_ID) {
        patchlib_uninstall_hook(g_hook_draw);
        g_hook_draw = PATCH_HOOK_INVALID_ID;
    }
}

static kernel_mod_info_t *get_info(void) { return (kernel_mod_info_t *)&g_info; }

__attribute__((visibility("default"))) kernel_mod_ops_t *create_kernel_mod(void) {
    static kernel_mod_ops_t ops;
    ops.init_mod = init_mod;
    ops.cleanup_mod = cleanup_mod;
    ops.get_info = get_info;
    return &ops;
}
