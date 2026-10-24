//
// 光影 Lights And Shadows —— KernelLoader (tefkernel) 模组，手机端 / PE
//
// 灵感来源：tModLoader 的「Lights And Shadows」(作者 yiyang233)。实现与参数自研。
// 感谢 yiyang233 大佬提供思路：先画光（中心取太阳位置）→ 蓝通道提阴影 → 沿太阳方向
// 缩放叠加 → 光减阴影 → additive 叠回。
//
// ---------------------------------------------------------------------------
// v2.4：仿原版的「光 − 阴影」屏幕后处理（感谢 yiyang233 大佬的思路）
// ---------------------------------------------------------------------------
//   绘制走原生 GL 屏幕后处理（eglSwapBuffers 钩子，在交换之前画）：
//     · 光：以太阳位置为中心的宽光晕（加色），颜色随一天时刻变化
//     · 阴影：本帧缩到 1/3，按蓝色通道提取遮挡（树/地面蓝少 → 阴影强）
//     · 光柱：阴影沿太阳方向逐层放大叠加成条纹，「光 − 阴影」漏出丁达尔光柱
//   光按 additive 叠回场景，不破坏 alpha。
//
//   太阳位置直接取游戏自己算好的 Main.LastCelestialBodyPosition，和天上那颗太阳对齐；
//   读不到才退回自己的圆弧（且不画亮核，避免出现第二颗太阳）。
//
//   状态由 Main.DrawInterface_36_Cursor 的 postfix 读出来写进快照（游戏线程），
//   渲染线程只读快照 —— 避免跨线程读游戏数据。
//
// 读属性的坑：稳定内核的 FFI 对**结构体返回**(Vector2) 有 bug（invoke 返回成功但不写缓冲）。
// 这里所有 0 参 getter 一律走 patchlib_method_get_pointer **原生直调**，绕开内核 FFI。
//

#include "mod-api/mod_core.h"
#include "mod-api/mod_logger.h"
#include "tefkernel/patchlib/type.h"
#include "tefkernel/patchlib/method.h"
#include "tefkernel/patchlib/field.h"

#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include <dlfcn.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

void (*mod_logger_write)(mod_log_level_t level, const char* tag, const char* fmt, ...) = NULL;

#define LOG_INFO(...)  do { if (mod_logger_write) mod_logger_write(MOD_LOG_LEVEL_INFO,  "LightsAndShadows", __VA_ARGS__); } while (0)
#define LOG_ERROR(...) do { if (mod_logger_write) mod_logger_write(MOD_LOG_LEVEL_ERROR, "LightsAndShadows", __VA_ARGS__); } while (0)

#define MOD_VERSION "2.4.0-light"

/* ============================================================================
 * 可调参数：<私有目录>/ls_settings.txt（每 2 秒热读）
 *   glow=0.60   太阳光（宽光晕）强度（0 = 关掉）
 *   rays=0.55   阴影条纹/光柱强度（0 = 关掉，只剩光晕）
 *   sun=-1      测试用：强制太阳时刻 0..1（<0 = 用游戏时间）
 * ========================================================================== */

static float g_cfg_glow = 0.60f;
static float g_cfg_rays = 0.55f;
static float g_cfg_sun = -1.0f;
static char  g_private_dir[1024];

static void read_settings(void) {
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
        float v = (float)atof(eq + 1);
        if (!strcmp(key, "glow")) g_cfg_glow = v;
        else if (!strcmp(key, "rays")) g_cfg_rays = v;
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

static bool main_static_slot(const char *name, void **slot) {
    patch_handle_t f = patchlib_type_get_field(g_main_type, name);
    if (!f) return false;
    *slot = patchlib_field_get_pointer(f, NULL);
    return *slot != NULL;
}

static void *main_getter_ptr(const char *name) {
    char buf[80];
    snprintf(buf, sizeof(buf), "get_%s", name);
    patch_handle_t g = patchlib_type_get_method_by_param_count(g_main_type, buf, 0);
    if (!g || !patchlib_is_valid(g)) return NULL;
    return patchlib_method_get_pointer(g);
}

static int main_int(const char *name, int fallback) {
    void *slot = NULL;
    if (main_static_slot(name, &slot)) return *(int *)slot;
    void *p = main_getter_ptr(name);
    if (p) return ((int (*)(void))p)();
    return fallback;
}

static bool main_bool(const char *name, bool fallback) {
    void *slot = NULL;
    if (main_static_slot(name, &slot)) return *(unsigned char *)slot != 0;
    void *p = main_getter_ptr(name);
    if (p) return ((int (*)(void))p)() != 0;
    return fallback;
}

static double main_double(const char *name, double fallback) {
    void *slot = NULL;
    if (main_static_slot(name, &slot)) return *(double *)slot;
    void *p = main_getter_ptr(name);
    if (p) return ((double (*)(void))p)();
    return fallback;
}

/* Vector2：优先读静态字段，否则原生直调 0 参 getter
 * （两个 float 的 HFA 结构体走 s0/s1 返回，和原生调用约定一致） */
static bool main_vec2(const char *name, float *x, float *y) {
    void *slot = NULL;
    if (main_static_slot(name, &slot)) {
        *x = ((float *)slot)[0];
        *y = ((float *)slot)[1];
        return true;
    }
    void *p = main_getter_ptr(name);
    if (!p) return false;
    v2_t v = ((v2_t (*)(void))p)();
    *x = v.x;
    *y = v.y;
    return true;
}

static bool player_center(float *cx, float *cy) {
    patch_handle_t player_type = patchlib_type_get_type("Terraria", "Player");
    if (!player_type) return false;
    void *player = NULL;
    patch_handle_t f_local = patchlib_type_get_field(g_main_type, "LocalPlayer");
    if (f_local) {
        void *slot = patchlib_field_get_pointer(f_local, NULL);
        if (slot) player = *(void **)slot;
    }
    if (!player) {
        void *p = main_getter_ptr("LocalPlayer");
        if (p) player = ((void *(*)(void))p)();
    }
    if (!player) return false;
    patch_handle_t f_pos = patchlib_type_get_field(player_type, "position");
    if (!f_pos) return false;
    float *pos = (float *)patchlib_field_get_pointer(f_pos, player);
    if (!pos) return false;
    int w = 20, h = 42;
    patch_handle_t f_w = patchlib_type_get_field(player_type, "width");
    patch_handle_t f_h = patchlib_type_get_field(player_type, "height");
    if (f_w) { int *p = (int *)patchlib_field_get_pointer(f_w, player); if (p) w = *p; }
    if (f_h) { int *p = (int *)patchlib_field_get_pointer(f_h, player); if (p) h = *p; }
    if (!isfinite(pos[0]) || !isfinite(pos[1])) return false;
    *cx = pos[0] + w * 0.5f;
    *cy = pos[1] + h * 0.5f;
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

    /* 太阳位置：直接用游戏自己算好的归一化位置，和天空里那颗太阳严丝合缝。
     * 拿不到再用圆弧兜底；兜底时不画实心亮核，免得天上出现第二颗太阳。 */
    float sx = 0.0f, sy = 0.0f;
    int exact = main_vec2("LastCelestialBodyPosition", &sx, &sy) &&
                !(sx == 0.0f && sy == 0.0f);
    if (exact) {
        /* 有的版本给的是像素坐标，按屏幕尺寸归一化 */
        if (sx < -1.5f || sx > 1.5f || sy < -1.5f || sy > 1.5f) {
            sx /= (float)sw;
            sy /= (float)sh;
        }
        if (!isfinite(sx) || !isfinite(sy) ||
            sx < -1.0f || sx > 2.0f || sy < -1.0f || sy > 2.0f) exact = 0;
    }
    if (exact) {
        s->sunU = sx;
        s->sunV = 1.0f - sy;
    } else {
        float arcW = sw * 0.5f + 160.0f;
        float baseY = sh * 0.62f;
        s->sunU = (sw * 0.5f - cosf(ang) * arcW) / (float)sw;
        s->sunV = 1.0f - (baseY - sinf(ang) * (sh * 0.42f)) / (float)sh;
    }
    s->sunExact = exact;
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

    /* 光色：按一天里的时刻插值（黎明淡紫 → 正午暖黄 → 黄昏橙红），夜里冷蓝 */
    if (day) {
        if (t01 < 0.5f) {
            float u = t01 * 2.0f;
            s->warmR = 0.73f + 0.13f * u;
            s->warmG = 0.54f + 0.22f * u;
            s->warmB = 0.58f - 0.06f * u;
        } else {
            float u = (t01 - 0.5f) * 2.0f;
            s->warmR = 0.86f - 0.14f * u;
            s->warmG = 0.76f - 0.46f * u;
            s->warmB = 0.52f - 0.14f * u;
        }
    } else {
        float w = (1.0f - moonK) * 0.5f;
        s->warmR = 0.42f + 0.24f * w;
        s->warmG = 0.52f + 0.16f * w;
        s->warmB = 0.92f;
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
 * 四、GL 层：仿原版「光 − 阴影」的屏幕后处理
 *
 *   1) 把本帧读成场景纹理（MSAA 用 glBlitFramebuffer 解析；不行再 glCopyTexSubImage2D）
 *   2) 场景缩到 1/3 分辨率，按蓝色通道取阴影：蓝越小阴影越强（天空是蓝的 → 不算阴影）
 *   3) 阴影沿太阳方向逐层放大叠加成条纹；以太阳为中心的宽光晕减掉条纹，
 *      再 additive 叠回场景 —— 树缝里就漏出光柱
 *
 *   读不了场景的设备回退纯叠加（光晕加色 + 色罩乘法），屏幕永远不会黑。
 *   g_mode: 0=未探测  1=后处理  2=纯叠加
 * ========================================================================== */

static const float QUAD[8] = { -1.f, -1.f, 1.f, -1.f, -1.f, 1.f, 1.f, 1.f };

static int g_gl_state;
static int g_mode;
static int g_use_blit;
static GLuint g_vbo;
static GLuint g_prog_post, g_prog_tint, g_prog_glow, g_prog_shadow;
static GLuint g_scene_tex, g_shadow_tex;
static GLuint g_fbo_scene, g_fbo_shadow;
static int g_scene_w, g_scene_h, g_shadow_w, g_shadow_h;

static GLint a_post, u_post_scene, u_post_shadow, u_post_res, u_post_sun,
             u_post_color, u_post_beam, u_post_rays, u_post_shadow_k, u_post_zoom;
static GLint a_tint, u_tint_center, u_tint_size, u_tint_res, u_tint_top, u_tint_bottom, u_tint_vignette;
static GLint a_glow, u_glow_center, u_glow_size, u_glow_res, u_glow_sun,
             u_glow_color, u_glow_strength;
static GLint a_shadow, u_shadow_scene, u_shadow_res, u_shadow_m;

#define GL_VERTEX_ARRAY_BINDING_OES 0x85B5
#define GL_FRAMEBUFFER_BINDING 0x8CA6
#define GL_FRAMEBUFFER         0x8D40
#define GL_COLOR_ATTACHMENT0   0x8CE0
#define GL_READ_FRAMEBUFFER    0x8CA8
#define GL_DRAW_FRAMEBUFFER    0x8CA9
#define GL_COLOR_BUFFER_BIT    0x00004000

typedef void (*PFN_bindvao)(GLuint);
typedef void (*PFN_genfb)(GLsizei, GLuint *);
typedef void (*PFN_bindfb)(GLenum, GLuint);
typedef void (*PFN_fbtex)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef void (*PFN_blit)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);

static PFN_bindvao p_bind_vao;
static PFN_genfb   p_gen_fb;
static PFN_bindfb  p_bind_fb;
static PFN_fbtex   p_fb_tex;
static PFN_blit    p_blit;

static void *proc(const char *a, const char *b) {
    void *p = (void *)eglGetProcAddress(a);
    if (p) return p;
    return (void *)eglGetProcAddress(b);
}

static const char *VS_SRC =
    "attribute vec2 aPos;\n"
    "varying vec2 vUV;\n"
    "uniform vec2 uCenter;\n"
    "uniform vec2 uSize;\n"
    "void main(){\n"
    "  vUV = aPos * 0.5 + 0.5;\n"
    "  gl_Position = vec4(uCenter + aPos * uSize, 0.0, 1.0);\n"
    "}\n";

/* —— 回退路径用：整屏色罩（乘法） —— */
static const char *FS_TINT =
    "precision mediump float;\n"
    "uniform vec2 uRes;\n"
    "uniform vec3 uTop;\n"
    "uniform vec3 uBottom;\n"
    "uniform float uVignette;\n"
    "void main(){\n"
    "  vec2 uv = gl_FragCoord.xy / uRes;\n"
    "  vec3 c = mix(uBottom, uTop, clamp(uv.y, 0.0, 1.0));\n"
    "  vec2 q = uv - 0.5;\n"
    "  float vig = 1.0 - uVignette * dot(q, q) * 3.0;\n"
    "  c *= clamp(vig, 0.0, 1.0);\n"
    "  gl_FragColor = vec4(c, 1.0);\n"
    "}\n";

/* —— 回退路径用：太阳光晕（加色，不读帧缓冲） —— */
static const char *FS_GLOW =
    "precision mediump float;\n"
    "uniform vec2 uRes;\n"
    "uniform vec2 uSun;\n"
    "uniform vec3 uColor;\n"
    "uniform float uStrength;\n"
    "void main(){\n"
    "  vec2 uv = gl_FragCoord.xy / uRes;\n"
    "  vec2 d = uv - uSun;\n"
    "  d.x *= uRes.x / uRes.y;\n"
    "  float r = length(d);\n"
    "  float fall = max(0.0, (3.0 - r) / 3.0);\n"
    "  gl_FragColor = vec4(uColor * fall * fall * uStrength, 1.0);\n"
    "}\n";

/* —— 阴影提取（1/3 分辨率）：蓝通道低于阈值 m 的像素算阴影 —— */
static const char *FS_SHADOW =
    "precision mediump float;\n"
    "uniform sampler2D uScene;\n"
    "uniform vec2 uRes;\n"
    "uniform float uM;\n"
    "void main(){\n"
    "  vec2 uv = gl_FragCoord.xy / uRes;\n"
    "  float b = texture2D(uScene, uv).b;\n"
    "  float s = (b < uM) ? pow(1.0 - b, 0.75) : 0.0;\n"
    "  gl_FragColor = vec4(s, s, s, 1.0);\n"
    "}\n";

/* —— 最终合成：光晕 − 沿太阳方向的阴影条纹，additive 叠回场景 —— */
static const char *FS_POST =
    "precision mediump float;\n"
    "uniform sampler2D uScene;\n"
    "uniform sampler2D uShadow;\n"
    "uniform vec2 uRes;\n"
    "uniform vec2 uSun;\n"
    "uniform vec3 uColor;\n"
    "uniform float uBeam;\n"
    "uniform float uRays;\n"
    "uniform float uShadowK;\n"
    "uniform float uZoom;\n"
    "void main(){\n"
    "  vec2 uv = gl_FragCoord.xy / uRes;\n"
    "  vec3 scene = texture2D(uScene, uv).rgb;\n"
    "  vec2 d = (uv - uSun) * vec2(uRes.x / uRes.y, 1.0);\n"
    "  float r = length(d);\n"
    "  float fall = max(0.0, (3.0 - r) / 3.0);\n"
    "  float streak = 0.0;\n"
    "  for (int i = 0; i < 30; ++i) {\n"
    "    float k = 1.0 + float(i) * uZoom;\n"
    "    vec2 p = uSun + (uv - uSun) / k;\n"
    "    streak += texture2D(uShadow, clamp(p, 0.0, 1.0)).r * (1.0 - float(i) / 30.0);\n"
    "  }\n"
    "  streak *= 0.055 * uShadowK * uRays;\n"
    "  vec3 light = uColor * fall * uBeam;\n"
    "  light = max(light - vec3(streak), 0.0);\n"
    "  gl_FragColor = vec4(scene + light, 1.0);\n"
    "}\n";

static GLuint compile_shader(GLenum type, const char *src, const char *tag) {
    GLuint sh = glCreateShader(type);
    if (!sh) return 0;
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        GLsizei n = 0;
        glGetShaderInfoLog(sh, sizeof(log) - 1, &n, log);
        log[n < 0 ? 0 : n] = 0;
        LOG_ERROR("着色器编译失败 %s: %s", tag, log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static GLuint build_program(const char *fsrc, const char *tag) {
    GLuint vs = compile_shader(GL_VERTEX_SHADER, VS_SRC, "VS");
    if (!vs) return 0;
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fsrc, tag);
    if (!fs) { glDeleteShader(vs); return 0; }
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        GLsizei n = 0;
        glGetProgramInfoLog(prog, sizeof(log) - 1, &n, log);
        log[n < 0 ? 0 : n] = 0;
        LOG_ERROR("程序链接失败 %s: %s", tag, log);
        glDeleteProgram(prog);
        return 0;
    }
    glDeleteShader(vs);
    glDeleteShader(fs);
    return prog;
}

static bool gl_build(void) {
    if (g_gl_state != 0) return g_gl_state > 0;

    g_prog_post   = build_program(FS_POST,   "光-阴影");
    g_prog_shadow = build_program(FS_SHADOW, "阴影");
    g_prog_tint   = build_program(FS_TINT,   "色罩");
    g_prog_glow   = build_program(FS_GLOW,   "光晕");
    if (!g_prog_post || !g_prog_shadow || !g_prog_tint || !g_prog_glow) {
        LOG_ERROR("GL 程序构建失败");
        g_gl_state = -1;
        return false;
    }

    a_post = glGetAttribLocation(g_prog_post, "aPos");
    u_post_scene    = glGetUniformLocation(g_prog_post, "uScene");
    u_post_shadow   = glGetUniformLocation(g_prog_post, "uShadow");
    u_post_res      = glGetUniformLocation(g_prog_post, "uRes");
    u_post_sun      = glGetUniformLocation(g_prog_post, "uSun");
    u_post_color    = glGetUniformLocation(g_prog_post, "uColor");
    u_post_beam     = glGetUniformLocation(g_prog_post, "uBeam");
    u_post_rays     = glGetUniformLocation(g_prog_post, "uRays");
    u_post_shadow_k = glGetUniformLocation(g_prog_post, "uShadowK");
    u_post_zoom     = glGetUniformLocation(g_prog_post, "uZoom");

    a_shadow      = glGetAttribLocation(g_prog_shadow, "aPos");
    u_shadow_scene = glGetUniformLocation(g_prog_shadow, "uScene");
    u_shadow_res   = glGetUniformLocation(g_prog_shadow, "uRes");
    u_shadow_m     = glGetUniformLocation(g_prog_shadow, "uM");

    a_tint = glGetAttribLocation(g_prog_tint, "aPos");
    u_tint_center = glGetUniformLocation(g_prog_tint, "uCenter");
    u_tint_size = glGetUniformLocation(g_prog_tint, "uSize");
    u_tint_res = glGetUniformLocation(g_prog_tint, "uRes");
    u_tint_top = glGetUniformLocation(g_prog_tint, "uTop");
    u_tint_bottom = glGetUniformLocation(g_prog_tint, "uBottom");
    u_tint_vignette = glGetUniformLocation(g_prog_tint, "uVignette");

    a_glow = glGetAttribLocation(g_prog_glow, "aPos");
    u_glow_center = glGetUniformLocation(g_prog_glow, "uCenter");
    u_glow_size = glGetUniformLocation(g_prog_glow, "uSize");
    u_glow_res = glGetUniformLocation(g_prog_glow, "uRes");
    u_glow_sun = glGetUniformLocation(g_prog_glow, "uSun");
    u_glow_color = glGetUniformLocation(g_prog_glow, "uColor");
    u_glow_strength = glGetUniformLocation(g_prog_glow, "uStrength");

    if (a_post < 0 || a_shadow < 0 || a_tint < 0 || a_glow < 0) {
        LOG_ERROR("aPos 无效");
        g_gl_state = -1;
        return false;
    }

    glGenBuffers(1, &g_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(QUAD), QUAD, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    GLuint tex[2] = { 0, 0 };
    glGenTextures(2, tex);
    g_scene_tex = tex[0];
    g_shadow_tex = tex[1];
    for (int i = 0; i < 2; ++i) {
        glBindTexture(GL_TEXTURE_2D, tex[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glBindTexture(GL_TEXTURE_2D, 0);

    p_bind_vao = (PFN_bindvao)proc("glBindVertexArrayOES", "glBindVertexArray");
    p_gen_fb   = (PFN_genfb)  proc("glGenFramebuffers", "glGenFramebuffersOES");
    p_bind_fb  = (PFN_bindfb) proc("glBindFramebuffer", "glBindFramebufferOES");
    p_fb_tex   = (PFN_fbtex)  proc("glFramebufferTexture2D", "glFramebufferTexture2DOES");
    p_blit     = (PFN_blit)   eglGetProcAddress("glBlitFramebuffer");

    g_fbo_scene = 0;
    g_fbo_shadow = 0;
    if (p_gen_fb && p_bind_fb && p_fb_tex) {
        GLuint fb[2] = { 0, 0 };
        p_gen_fb(2, fb);
        g_fbo_scene = fb[0];
        g_fbo_shadow = fb[1];
    }

    g_gl_state = 1;
    LOG_INFO("GL 就绪 v%s（blit=%d fbo=%u/%u）", MOD_VERSION,
             p_blit ? 1 : 0, g_fbo_scene, g_fbo_shadow);
    return true;
}

/* 把默认帧缓冲读进场景纹理；1=成功 */
static int fill_scene(int W, int H) {
    glBindTexture(GL_TEXTURE_2D, g_scene_tex);
    if (g_scene_w != W || g_scene_h != H) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        g_scene_w = W;
        g_scene_h = H;
        if (p_bind_fb && g_fbo_scene) {
            p_bind_fb(GL_FRAMEBUFFER, g_fbo_scene);
            p_fb_tex(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_scene_tex, 0);
            p_bind_fb(GL_FRAMEBUFFER, 0);
        }
    }

    glGetError();
    if (g_use_blit && p_blit && g_fbo_scene) {
        p_bind_fb(GL_READ_FRAMEBUFFER, 0);
        p_bind_fb(GL_DRAW_FRAMEBUFFER, g_fbo_scene);
        p_blit(0, 0, W, H, 0, 0, W, H, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        p_bind_fb(GL_FRAMEBUFFER, 0);
        if (glGetError() == GL_NO_ERROR) return 1;
        glGetError();
        g_use_blit = 0;
    }

    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, W, H);
    if (glGetError() == GL_NO_ERROR) return 1;
    return 0;
}

/* 探测：MSAA 解析 → 直接拷贝 → 纯叠加 */
static int gl_probe_mode(int W, int H) {
    if (p_blit && g_fbo_scene) {
        g_use_blit = 1;
        if (fill_scene(W, H)) {
            LOG_INFO("MSAA 解析成功，启用体积光");
            return 1;
        }
        LOG_INFO("MSAA 解析失败，改试直接拷贝");
    }
    g_use_blit = 0;
    if (fill_scene(W, H)) {
        LOG_INFO("场景拷贝成功，启用体积光");
        return 1;
    }
    LOG_ERROR("读场景失败，回退到纯叠加");
    return 2;
}

static void bind_quad(GLuint prog, GLint apos, GLint ucen, GLint usz, float hw, float hh) {
    glUseProgram(prog);
    if (ucen >= 0) glUniform2f(ucen, 0.f, 0.f);
    if (usz >= 0) glUniform2f(usz, hw, hh);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glEnableVertexAttribArray((GLuint)apos);
    glVertexAttribPointer((GLuint)apos, 2, GL_FLOAT, GL_FALSE, 0, 0);
}

static void gl_render(const light_snap_t *s) {
    if (!s->enabled) return;
    if (!gl_build()) return;

    GLint vp[4] = { 0, 0, 0, 0 };
    glGetIntegerv(GL_VIEWPORT, vp);
    GLint W = vp[2], H = vp[3];
    if (W <= 0 || H <= 0) return;

    static int first = 1;
    if (first) { first = 0; LOG_INFO("首次交换绘制：视口=%dx%d", W, H); }

    GLint pProg = 0, pTex = 0, pAct = 0, pVao = 0, pBuf = 0, pFbo = 0;
    GLint pBlend = 0, pDepth = 0, pCull = 0, pScissor = 0;
    GLint pSrcRGB = 0, pDstRGB = 0, pEqRGB = 0, pSrcA = 0, pDstA = 0, pEqA = 0;
    GLint pVp[4] = { 0, 0, 0, 0 };
    glGetIntegerv(GL_CURRENT_PROGRAM, &pProg);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &pTex);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &pAct);
    if (p_bind_vao) glGetIntegerv(GL_VERTEX_ARRAY_BINDING_OES, &pVao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &pBuf);
    if (p_bind_fb) glGetIntegerv(GL_FRAMEBUFFER_BINDING, &pFbo);
    glGetIntegerv(GL_BLEND, &pBlend);
    glGetIntegerv(GL_DEPTH_TEST, &pDepth);
    glGetIntegerv(GL_CULL_FACE, &pCull);
    glGetIntegerv(GL_SCISSOR_TEST, &pScissor);
    glGetIntegerv(GL_BLEND_SRC_RGB, &pSrcRGB);
    glGetIntegerv(GL_BLEND_DST_RGB, &pDstRGB);
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &pEqRGB);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &pSrcA);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &pDstA);
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &pEqA);
    memcpy(pVp, vp, sizeof(vp));

    if (p_bind_vao) p_bind_vao(0);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, W, H);
    glActiveTexture(GL_TEXTURE0);

    if (g_mode == 0) g_mode = gl_probe_mode(W, H);

    if (g_mode == 1 && fill_scene(W, H)) {
        /* —— 阴影：场景缩到 1/3，按蓝色通道取阴影 —— */
        int sw = W / 3, sh = H / 3;
        if (sw < 1) sw = 1;
        if (sh < 1) sh = 1;
        if (g_fbo_shadow && p_bind_fb) {
            glBindTexture(GL_TEXTURE_2D, g_shadow_tex);
            if (g_shadow_w != sw || g_shadow_h != sh) {
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, sw, sh, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
                g_shadow_w = sw;
                g_shadow_h = sh;
                p_bind_fb(GL_FRAMEBUFFER, g_fbo_shadow);
                p_fb_tex(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_shadow_tex, 0);
            }
            p_bind_fb(GL_FRAMEBUFFER, g_fbo_shadow);
            glViewport(0, 0, sw, sh);
            glDisable(GL_BLEND);
            bind_quad(g_prog_shadow, a_shadow, -1, -1, 1.f, 1.f);
            glUniform2f(u_shadow_res, (float)sw, (float)sh);
            glUniform1f(u_shadow_m, s->shadowM);
            glUniform1i(u_shadow_scene, 0);
            glBindTexture(GL_TEXTURE_2D, g_scene_tex);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            p_bind_fb(GL_FRAMEBUFFER, 0);
            glViewport(0, 0, W, H);
        }

        /* —— 合成：光晕 − 条纹，additive 叠回 —— */
        glDisable(GL_BLEND);
        bind_quad(g_prog_post, a_post, -1, -1, 1.f, 1.f);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, g_scene_tex);
        glUniform1i(u_post_scene, 0);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, g_shadow_tex);
        glUniform1i(u_post_shadow, 1);
        glUniform2f(u_post_res, (float)W, (float)H);
        glUniform2f(u_post_sun, s->sunU, s->sunV);
        glUniform3f(u_post_color, s->warmR, s->warmG, s->warmB);
        glUniform1f(u_post_beam, s->glowA);
        glUniform1f(u_post_rays, g_cfg_rays);
        glUniform1f(u_post_shadow_k, s->shadowK);
        glUniform1f(u_post_zoom, 0.111f);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE0);
    } else {
        /* —— 纯叠加：光晕加色 + 色罩乘法（不读帧缓冲） —— */
        glEnable(GL_BLEND);
        glBlendEquation(GL_FUNC_ADD);

        glBlendFuncSeparate(GL_DST_COLOR, GL_ZERO, GL_ZERO, GL_ONE);
        bind_quad(g_prog_tint, a_tint, u_tint_center, u_tint_size, 1.f, 1.f);
        glUniform2f(u_tint_res, (float)W, (float)H);
        glUniform3f(u_tint_top, s->topR, s->topG, s->topB);
        glUniform3f(u_tint_bottom, s->botR, s->botG, s->botB);
        glUniform1f(u_tint_vignette, s->vignette);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        if (s->glowA > 0.004f && s->sunU > -0.35f && s->sunU < 1.35f &&
            s->sunV > -0.35f && s->sunV < 1.35f) {
            glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ZERO, GL_ONE);
            bind_quad(g_prog_glow, a_glow, u_glow_center, u_glow_size, 1.f, 1.f);
            glUniform2f(u_glow_res, (float)W, (float)H);
            glUniform2f(u_glow_sun, s->sunU, s->sunV);
            glUniform3f(u_glow_color, s->warmR, s->warmG, s->warmB);
            glUniform1f(u_glow_strength, s->glowA);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        }
    }

    if (p_bind_fb) p_bind_fb(GL_FRAMEBUFFER, (GLuint)pFbo);
    if (p_bind_vao && pVao) p_bind_vao((GLuint)pVao);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)pBuf);
    glUseProgram((GLuint)pProg);
    glActiveTexture((GLuint)pAct);
    glBindTexture(GL_TEXTURE_2D, (GLuint)pTex);
    glBlendEquationSeparate((GLenum)pEqRGB, (GLenum)pEqA);
    glBlendFuncSeparate((GLenum)pSrcRGB, (GLenum)pDstRGB, (GLenum)pSrcA, (GLenum)pDstA);
    if (pBlend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (pDepth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (pCull) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    if (pScissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    glViewport(pVp[0], pVp[1], pVp[2], pVp[3]);
}

/* ============================================================================
 * 五、eglSwapBuffers 钩子
 * ========================================================================== */

typedef EGLBoolean (*PFN_swap)(EGLDisplay, EGLSurface);
static PFN_swap g_real_swap;
static unsigned char g_saved16[16], g_patch16[16];
static volatile int g_in_hook;
static bool g_hook_installed;

#define PAGE_SZ 4096

static int make_rwx(void *addr, size_t len) {
    uintptr_t p = (uintptr_t)addr & ~(uintptr_t)(PAGE_SZ - 1);
    uintptr_t e = ((uintptr_t)addr + len + PAGE_SZ - 1) & ~(uintptr_t)(PAGE_SZ - 1);
    return mprotect((void *)p, e - p, PROT_READ | PROT_WRITE | PROT_EXEC);
}

static void patch_bytes(void *dst, const void *src) {
    if (make_rwx(dst, 16) == 0) {
        memcpy(dst, src, 16);
        __builtin___clear_cache((char *)dst, (char *)dst + 16);
    }
}

static EGLBoolean hook_swap(EGLDisplay dpy, EGLSurface surf) {
    if (!g_in_hook) {
        g_in_hook = 1;
        int i = g_snap_head;
        if (i >= 0) gl_render(&g_snap[i]);
        g_in_hook = 0;
    }
    patch_bytes((void *)g_real_swap, g_saved16);
    EGLBoolean r = g_real_swap(dpy, surf);
    patch_bytes((void *)g_real_swap, g_patch16);
    return r;
}

static bool install_swap_hook(void) {
    void *egl = dlopen("libEGL.so", RTLD_NOW);
    if (!egl) { LOG_ERROR("dlopen(libEGL.so) 失败"); return false; }
    g_real_swap = (PFN_swap)dlsym(egl, "eglSwapBuffers");
    if (!g_real_swap) { LOG_ERROR("找不到 eglSwapBuffers"); return false; }

    uint32_t *p = (uint32_t *)g_patch16;
    p[0] = 0x58000050u;
    p[1] = 0xD61F0200u;
    *(uint64_t *)(g_patch16 + 8) = (uint64_t)(uintptr_t)hook_swap;
    memcpy(g_saved16, (void *)g_real_swap, 16);

    if (make_rwx((void *)g_real_swap, 16) != 0) { LOG_ERROR("mprotect 失败"); return false; }
    memcpy((void *)g_real_swap, g_patch16, 16);
    __builtin___clear_cache((char *)g_real_swap, (char *)g_real_swap + 16);
    g_hook_installed = true;
    LOG_INFO("已挂 eglSwapBuffers=%p", (void *)g_real_swap);
    return true;
}

static void uninstall_swap_hook(void) {
    if (g_hook_installed && g_real_swap) {
        patch_bytes((void *)g_real_swap, g_saved16);
        g_hook_installed = false;
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
        LOG_INFO("太阳uvx1000=(%d,%d) 真实=%d 光强x100=%d 光柱x100=%d 阴影x100=%d 模式=%d",
                 (int)(s->sunU * 1000), (int)(s->sunV * 1000), s->sunExact,
                 (int)(s->glowA * 100), (int)(g_cfg_rays * 100),
                 (int)(s->shadowK * 100), g_mode);
    }
}

/* ============================================================================
 * 七、入口
 * ========================================================================== */

static const kernel_mod_info_t g_info = {
    .pkg_id = "lzup.lightsandshadows",
    .version_code = 9,
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

    if (!install_swap_hook()) LOG_ERROR("eglSwapBuffers 钩子安装失败");

    read_settings();
    LOG_INFO("初始化完成 v%s（快照 hook=%d swap=%d）", MOD_VERSION, (int)g_hook_draw,
             g_hook_installed ? 1 : 0);
}

static void cleanup_mod(kernel_mod_handle_t *handle) {
    (void)handle;
    uninstall_swap_hook();
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
