/* Android ARM64 world lighting. Native SpriteBatch ABI follows LuaLoader.
 * Do not create textures at init, draw during splash, or nest Begin calls. */
#include "mod-api/mod_core.h"
#include "mod-api/mod_logger.h"
#include "tefkernel/patchlib/type.h"
#include "tefkernel/patchlib/method.h"
#include "tefkernel/patchlib/field.h"
#include "tefkernel/terraria/texture2d.h"

#include <dlfcn.h>
#include "xdl.h"   /* MIT: HexHacking Team —— dlopen 拿不到时的符号查找回退 */
#include <stdio.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* KernelLoader injects this slot by dlsym before init_mod.
 * It MUST stay exported even if internal implementation visibility changes. */
__attribute__((visibility("default"))) void (*mod_logger_write)(mod_log_level_t level, const char* tag, const char* fmt, ...) = NULL;

#define LOG_INFO(...)  do { if (mod_logger_write) mod_logger_write(MOD_LOG_LEVEL_INFO,  "LightsAndShadows", __VA_ARGS__); } while (0)
#define LOG_ERROR(...) do { if (mod_logger_write) mod_logger_write(MOD_LOG_LEVEL_ERROR, "LightsAndShadows", __VA_ARGS__); } while (0)

/* ============================ 绘制层 ============================ */

typedef struct { float x, y; } v2_t;
typedef struct { int x, y, w, h; } rect_t;
typedef struct { unsigned char has, pad0, pad1, pad2; float m[16]; } nullable_mat_t;

static void *make_texture(int size, float (*alpha_at)(float));


static int g_frames;

static patch_handle_t g_main_type;

#define LIGHTS_TEX 256   /* 借游戏批次绘制时采样是 PointClamp，贴图必须够大才不成方块 */
static float alpha_soft(float r);


static patch_handle_t g_sb_field;        /* Main.spriteBatch */
static patch_handle_t g_begin9, g_end0, g_draw9;
static void *g_blend_additive;          /* BlendState.Additive：光要"加"上去，不能盖住背景 */
static void *nat_begin, *nat_draw;
static void *g_white;                    /* 1x1 白，画实心矩形用 */
static void *g_soft;            /* 光晕 / 压暗环贴图 */

/* ============================ GC 根 ============================ */
/* terraria_texture2d_create 返回纹理对象**不带 GC 根**，Unity 的 GC 可以把
 * 它挪走/回收，之后我们再拿这个裸指针去画，画出来的就是别的对象（真机现象：
 * "画面上出现游戏里别的贴图"）。所以每个创建出来的对象都要钉一个 gchandle。
 * 符号从 libil2cpp.so 里 dlsym（和 LuaLoader 一样）。 */
static uint32_t (*g_gchandle_new)(void *obj, bool pinned) = NULL;
static void (*g_gchandle_free)(uint32_t) = NULL;
static uint32_t texture_roots[2];
static int root_count;

/* 从 /proc/self/maps 里扒出真实路径（库常是按全路径映射的 soname 查不到；
 * 另外内核会把模块文件标记删除，maps 里会带 " (deleted)"，要剥掉） */
static void *dlopen_by_maps(const char *name) {
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return NULL;
    char line[1024], found[512];
    void *handle = NULL;
    found[0] = 0;
    while (fgets(line, sizeof(line), f)) {
        char *slash = strchr(line, '/');
        if (!slash) continue;
        size_t n = strlen(slash);
        while (n && (slash[n-1] == '\n' || slash[n-1] == '\r' || slash[n-1] == ' ')) slash[--n] = 0;
        const char *deleted = " (deleted)";
        size_t dl = strlen(deleted);
        if (n > dl && strcmp(slash + n - dl, deleted) == 0) slash[n - dl] = 0;
        size_t nl = strlen(name);
        if (n >= nl && strcmp(slash + n - nl, name) == 0) {
            strncpy(found, slash, sizeof(found) - 1);
            found[sizeof(found) - 1] = 0;
            handle = dlopen(found, RTLD_NOW);
            break;
        }
    }
    fclose(f);
    if (!handle) LOG_INFO("maps 里没找到 %s（最后见到 %s）", name, found[0] ? found : "无");
    return handle;
}

static bool pin_object(void *obj) {
    if (!obj) return false;
    if (!g_gchandle_new) {
        void *lib = dlopen("libil2cpp.so", RTLD_NOW | RTLD_NOLOAD);
        if (!lib) lib = dlopen("libil2cpp.so", RTLD_NOW);
        if (!lib) lib = dlopen_by_maps("libil2cpp.so");
        if (lib) g_gchandle_new = (uint32_t (*)(void *, bool))dlsym(lib, "il2cpp_gchandle_new");
        if (!g_gchandle_new)
            g_gchandle_new = (uint32_t (*)(void *, bool))dlsym(RTLD_DEFAULT, "il2cpp_gchandle_new");
        if (!g_gchandle_new) {
            /* 照抄 LuaLoader：xdl 从内存里的 ELF 解析，绕开 dlopen 的限制 */
            void *ext = xdl_open("libil2cpp.so", XDL_DEFAULT);
            if (ext) g_gchandle_new = (uint32_t (*)(void *, bool))xdl_sym(ext, "il2cpp_gchandle_new", NULL);
            LOG_INFO("xdl_open=%p gchandle_new=%p", ext, (void *)g_gchandle_new);
        }
        LOG_INFO("il2cpp_gchandle_new = %p (lib=%p)", (void *)g_gchandle_new, lib);
    }
    if (!g_gchandle_new) return false;
    uint32_t root=g_gchandle_new(obj, false);
    if(!root) return false;
    if(root_count<2) texture_roots[root_count++]=root;
    if(!g_gchandle_free) {
        void *ext=xdl_open("libil2cpp.so",XDL_DEFAULT);
        if(ext) g_gchandle_free=(void(*)(uint32_t))xdl_sym(ext,"il2cpp_gchandle_free",NULL);
    }
    return true;
}

/* 0xRRGGBBAA -> PE 的预乘 packed（A 在最低字节、RGB 已乘 A） */
static uint32_t pack_color(uint32_t rgba) {
    unsigned char r = (unsigned char)(rgba >> 24), g = (unsigned char)(rgba >> 16);
    unsigned char b = (unsigned char)(rgba >> 8), a = (unsigned char)rgba;
    unsigned int pb = ((unsigned)b * a + 127) / 255, pg = ((unsigned)g * a + 127) / 255, pr = ((unsigned)r * a + 127) / 255;
    return (uint32_t)(a | (pb << 8) | (pg << 16) | (pr << 24));
}

static void *sprite_batch(void) {
    void *slot = g_sb_field ? patchlib_field_get_pointer(g_sb_field, NULL) : NULL;
    return slot ? *(void **)slot : NULL;
}

static bool draw_init(void) {
    patch_handle_t sb_type = patchlib_type_get_type("Microsoft.Xna.Framework.Graphics", "SpriteBatch");
    patch_handle_t main_type = patchlib_type_get_type("Terraria", "Main");
    if (!sb_type || !main_type) return false;

    g_sb_field = patchlib_type_get_field(main_type, "spriteBatch");
    g_begin9 = patchlib_type_get_method_by_param_count(sb_type, "Begin", 8);
    g_end0   = patchlib_type_get_method_by_param_count(sb_type, "End", 0);
    g_draw9  = patchlib_type_get_method_by_param_count(sb_type, "Draw", 9);
    if (!g_begin9 || !g_end0 || !g_draw9) return false;
    /* BlendState.Additive（静态字段）——光加色叠加才是提亮而不是盖住背景 */
    {
        patch_handle_t bs_type = patchlib_type_get_type("Microsoft.Xna.Framework.Graphics", "BlendState");
        /* 字段优先，取不到再试属性 get_Additive（PE 上很多静态量是属性） */
        patch_handle_t f_add = bs_type ? patchlib_type_get_field(bs_type, "Additive") : NULL;
        void *slot = f_add ? patchlib_field_get_pointer(f_add, NULL) : NULL;
        if (slot) g_blend_additive = *(void **)slot;
        if (!g_blend_additive && bs_type) {
            patch_handle_t getter = patchlib_type_get_method_by_param_count(bs_type, "get_Additive", 0);
            if (getter && patchlib_is_valid(getter)) {
                void *obj = NULL;
                if (patchlib_method_invoke_args(getter, NULL, &obj, NULL)) g_blend_additive = obj;
            }
        }
        LOG_INFO("BlendState.Additive = %p", g_blend_additive);
    }

    nat_begin = patchlib_method_get_pointer(g_begin9);
    nat_draw  = patchlib_method_get_pointer(g_draw9);
    if (!nat_begin || !nat_draw) { LOG_ERROR("取原生指针失败"); return false; }

    /* ⚠ 贴图**不在这里**建：init_mod 发生在内核初始化阶段，那时 Unity 还没跑起来，
     *   terraria_texture2d_create → Texture2D..ctor 会让 il2cpp 直接 abort（真机实测崩溃）。
     *   白贴图/光晕贴图都延迟到进游戏后的第一帧再建。 */
    return true;
}

/* 延迟建贴图（必须在游戏跑起来之后、游戏线程上） */
static bool ensure_textures(void) {
    if (g_white && g_soft) return true;
    if (g_white || g_soft) return false;
    const unsigned char px[4] = { 255, 255, 255, 255 };
    g_white = terraria_texture2d_create(1, 1, TEXTURE_FORMAT_RGBA32, (void*)px, 4);
    if (!g_white) return false;
    if (!pin_object(g_white)) { g_white=NULL; return false; }
    g_soft = make_texture(LIGHTS_TEX, alpha_soft);
    return g_soft != NULL;
}

static void draw_begin_blend(void *blend) {
    void *sb = sprite_batch();
    if (!sb || !nat_begin) return;
    nullable_mat_t ident; memset(&ident, 0, sizeof(ident));
    ident.has = 1; ident.m[0] = ident.m[5] = ident.m[10] = ident.m[15] = 1.0f;
    ((void (*)(void *, int, void *, void *, void *, void *, void *, nullable_mat_t *, bool))nat_begin)
        (sb, 0, blend, NULL, NULL, NULL, NULL, &ident, true);
}

static void draw_begin(void) { draw_begin_blend(NULL); }

static void draw_end(void) {
    void *sb = sprite_batch();
    if (!sb || !g_end0) return;
    patchlib_method_invoke_args(g_end0, sb, NULL, NULL);
}

static void draw_tex(void *tex, float x, float y, float w, float h, float tw, float th, uint32_t rgba) {
    void *sb = sprite_batch();
    if (!sb || !tex || !nat_draw || tw <= 0 || th <= 0) return;
    rect_t src = { 0, 0, (int)tw, (int)th };
    v2_t pos = { x, y }, origin = { 0, 0 }, scale = { w / tw, h / th };
    ((void (*)(void *, void *, v2_t, rect_t *, uint32_t, float, v2_t, v2_t, int, float))nat_draw)
        (sb, tex, pos, &src, pack_color(rgba), 0.0f, origin, scale, 0, 0.0f);
}

static void draw_rect(float x, float y, float w, float h, uint32_t rgba) { draw_tex(g_white, x, y, w, h, 1, 1, rgba); }

/* 程序化贴图：RGB 预乘（= A），否则画出来发白 */
static void *make_texture(int size, float (*alpha_at)(float)) {
    unsigned char *px = (unsigned char *)malloc((size_t)size * size * 4);
    if (!px) return NULL;
    float half = (size - 1) * 0.5f;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            float dx = (x - half) / half, dy = (y - half) / half;
            float r = sqrtf(dx * dx + dy * dy); if (r > 1.0f) r = 1.0f;
            float a = alpha_at(r); if (a < 0) a = 0; if (a > 1) a = 1;
            unsigned char v = (unsigned char)(a * 255.0f);
            unsigned char *p = px + ((size_t)y * size + x) * 4;
            p[0] = v; p[1] = v; p[2] = v; p[3] = v;
        }
    }
    void *tex = terraria_texture2d_create(size, size, TEXTURE_FORMAT_RGBA32, px, (size_t)size * size * 4);
    free(px);
    return tex;
}

/* 柔光斑衰减：指数 2.0 -> 1.6（衰减慢 20%，光铺得更开） */
static float alpha_soft(float r) {
    float k = 1.0f - r;
    if (k < 0.0f) k = 0.0f;
    k = powf(k, 1.6f);
    return k > 1.0f ? 1.0f : k;
}
/* ============================ Main 取值 ============================ */
/* PE 上 screenWidth/screenHeight/mouseX/mouseY/gameMenu 是**属性**；dayTime/time 等是字段 */

typedef struct { char name[64]; patch_handle_t field,getter; } main_access;
static main_access main_cache[24];
static int main_cache_count;
static main_access *main_access_for(const char *name) {
    for(int i=0;i<main_cache_count;i++) if(!strcmp(main_cache[i].name,name)) return &main_cache[i];
    if(main_cache_count>=24) return NULL;
    main_access *a=&main_cache[main_cache_count++];
    snprintf(a->name,sizeof(a->name),"%s",name);
    a->field=patchlib_type_get_field(g_main_type,name);
    char buf[80];snprintf(buf,sizeof(buf),"get_%s",name);
    a->getter=patchlib_type_get_method_by_param_count(g_main_type,buf,0);
    return a;
}
static patch_handle_t main_getter(const char *name) {
    main_access *a=main_access_for(name);return a?a->getter:NULL;
}
static bool main_static_slot(const char *name, void **slot) {
    main_access *a=main_access_for(name);
    patch_handle_t f = a?a->field:NULL;
    if (!f) return false;
    *slot = patchlib_field_get_pointer(f, NULL);
    return *slot != NULL;
}

static int main_int(const char *name, int fallback) {
    void *slot = NULL;
    if (main_static_slot(name, &slot)) return *(int *)slot;
    patch_handle_t g = main_getter(name);
    if (g && patchlib_is_valid(g)) {
        int out = fallback;
        if (patchlib_method_invoke_args(g, NULL, &out, NULL)) return out;
    }
    return fallback;
}

static double main_double(const char *name, double fallback) {
    void *slot = NULL;
    if (main_static_slot(name, &slot)) return *(double *)slot;
    patch_handle_t g = main_getter(name);
    if (g && patchlib_is_valid(g)) {
        double out = fallback;
        if (patchlib_method_invoke_args(g, NULL, &out, NULL)) return out;
    }
    return fallback;
}

/* 取静态属性返回的 Vector2（8 字节结构，内核把返回值写进缓冲） */
static bool main_vec2(const char *name, float *x, float *y) {
    patch_handle_t g = main_getter(name);
    if (!g || !patchlib_is_valid(g)) return false;
    float v[2] = { 0, 0 };
    if (!patchlib_method_invoke_args(g, NULL, v, NULL)) return false;
    *x = v[0]; *y = v[1];
    return true;
}

static bool main_bool(const char *name, bool fallback) {
    void *slot = NULL;
    if (main_static_slot(name, &slot)) return *(unsigned char *)slot != 0;
    patch_handle_t g = main_getter(name);
    if (g && patchlib_is_valid(g)) {
        int out = fallback ? 1 : 0;
        if (patchlib_method_invoke_args(g, NULL, &out, NULL)) return out != 0;
    }
    return fallback;
}


#include "world_occlusion.h"
/* Observe the game's exact Begin arguments. Suspend and restore only a known
 * active batch; never blindly Begin or assume the UI batch transform. */
typedef struct {
    void *sb,*blend,*sampler,*depth,*raster,*effect;
    int sort; bool deferred,valid; nullable_mat_t matrix;
} batch_state;
static batch_state observed;
static bool own_batch;
static patch_handle_t begin_called;
static patch_hook_id_t hooks[4];
static int hook_count;
static void observe_begin(patch_handle_t instance,void **args,void *result,const patch_method_signature_t *sig) {
    (void)result;(void)sig;
    if(own_batch || !args) return;
    for(int i=0;i<8;i++) if(!args[i]) { observed.valid=false;return; }
    observed.sb=instance; observed.sort=*(int*)args[0];
    observed.blend=*(void**)args[1]; observed.sampler=*(void**)args[2];
    observed.depth=*(void**)args[3]; observed.raster=*(void**)args[4];
    observed.effect=*(void**)args[5]; memcpy(&observed.matrix,args[6],sizeof(nullable_mat_t));
    observed.deferred=*(bool*)args[7]; observed.valid=true;
}
static void observe_short_begin(patch_handle_t instance,void **args,void *result,const patch_method_signature_t *sig) {
    (void)instance;(void)args;(void)result;(void)sig;
    if(!own_batch) observed.valid=false;
}
static void observe_end(patch_handle_t instance,void **args,void *result,const patch_method_signature_t *sig) {
    (void)args;(void)result;(void)sig;
    if(!own_batch && observed.sb==instance) observed.valid=false;
}
static void restore_batch(const batch_state *s) {
    ((void (*)(void*,int,void*,void*,void*,void*,void*,nullable_mat_t*,bool))nat_begin)
        (s->sb,s->sort,s->blend,s->sampler,s->depth,s->raster,s->effect,(nullable_mat_t*)&s->matrix,s->deferred);
}
/* PE uses virtual screen dimensions for zoom. screenPosition is the world
 * origin of that virtual viewport. Do not apply GameViewMatrix a second time. */
static void virtual_view_matrix(float m[16]) {
    memset(m,0,16*sizeof(float));m[0]=m[5]=m[10]=m[15]=1.f;
}
/* Sparse world-space grid: bounded ray budget, no per-frame texture upload.
 * Each cell is classified independently, so entering a house never toggles
 * sunlight for the entire screen. Cache follows tile coordinates, not player. */
#define GRID_MAX 24000
static struct { int x,y; float shade; unsigned short header; bool rim; } grid[GRID_MAX];
static int grid_count,grid_x,grid_y,grid_cols,grid_rows;
static int grid_dir;
static float grid_slope;
static float previous_cam_x,previous_cam_y;
static bool had_camera;
static void render_world(int sw,int sh,float cx,float cy,const float m[16],bool day,float t) {
    float lift=smooth01(sinf(3.14159265f*clamp01(t)));
    float slope=0.30f+2.8f*lift;
    int dir=t<0.5f?1:-1;
    float zx=m[0],zy=m[5];
    if(zx<=0 || zy<=0) return;
    float left=cx-m[12]/zx,top=cy-m[13]/zy;
    int x0=(int)floorf(left/16.f)-1,y0=(int)floorf(top/16.f)-1;
    int cols=(int)ceilf(sw/zx/16.f)+3,rows=(int)ceilf(sh/zy/16.f)+3;
    if(cols<=0 || rows<=0 || cols>512 || rows>512 || (size_t)cols*rows>GRID_MAX) return;
    bool reset=!had_camera || fabsf(cx-previous_cam_x)>128 || fabsf(cy-previous_cam_y)>128;
    previous_cam_x=cx;previous_cam_y=cy;had_camera=true;
    bool refresh=reset || g_frames%4==0 || x0!=grid_x || y0!=grid_y || cols!=grid_cols || rows!=grid_rows || dir!=grid_dir || fabsf(slope-grid_slope)>0.08f;
    if(refresh) {
        bool same=!reset && x0==grid_x && y0==grid_y && cols==grid_cols && rows==grid_rows;
        grid_x=x0;grid_y=y0;grid_cols=cols;grid_rows=rows;grid_dir=dir;grid_slope=slope;
        grid_count=0;
        double surface=main_double("worldSurface",0);
        for(int iy=0;iy<rows;iy++) for(int ix=0;ix<cols;ix++) {
            int x=x0+ix,y=y0+iy;unsigned short h=0;
            int solid=blocking_tile(x,y,&h);
            float shade=0;bool exposed=false;
            /* Sunlight does not originate in deep caves. Keep vanilla lighting. */
            if(solid>=0 && surface>0 && y<surface+8) {
                int sky=0;
                for(int k=1;k<=48;k++) {
                    int b=blocking_tile(x,y-k,NULL);
                    if(b<0) break;
                    if(b) { sky=1;break; }
                }
                if(!sky) {
                    exposed=true;
                    float origin_y=ray_origin_y(y,h,solid);
                    int hit=0;
                    for(int k=1;k<=24;k++) {
                        int b=ray_blocked(x+0.5f-dir*k,origin_y-k*slope);
                        if(b<0) break;
                        if(b) { hit=k;break; }
                    }
                    if(hit) shade=(0.08f+0.17f*(1.f-hit/25.f))*(day?1.f:0.4f);
                    /* Contact shade below ledges, only in open air. */
                } else if(!solid && blocking_tile(x,y-1,NULL)>0) shade=day?0.12f:0.05f;
            }
            int i=grid_count++;
            grid[i].x=x;grid[i].y=y;grid[i].header=h;
            grid[i].rim=exposed && solid>0 && shade<0.006f && blocking_tile(x,y-1,NULL)==0;
            grid[i].shade=same?grid[i].shade+(shade-grid[i].shade)*0.38f:shade;
        }
    }
    for(int i=0;i<grid_count;i++) {
        float a=grid[i].shade;if(a<0.006f) continue;
        float x=(grid[i].x*16.f-cx)*zx+m[12],y=(grid[i].y*16.f-cy)*zy+m[13];
        draw_rect(x,y,16.f*zx+0.25f,16.f*zy+0.25f,0x080f2000u|(uint32_t)(clamp01(a)*255));
    }
    if(g_blend_additive) {
        draw_end();draw_begin_blend(g_blend_additive);
        float strength=day?(0.07f+0.03f*lift):0.025f;
        uint32_t rgb=day?0xffdcaa00u:0x99bbee00u;
        for(int i=0;i<grid_count;i++) if(grid[i].rim) {
            float x=(grid[i].x*16.f-cx)*zx+m[12];
            float base=(grid[i].y*16.f-cy)*zy+m[13];
            /* Four strips approximate a slope without outlining empty pixels. */
            for(int k=0;k<4;k++) {
                float y=base+tile_surface(grid[i].header,(k+0.5f)/4.f)*16.f*zy;
                draw_rect(x+k*4.f*zx,y,4.f*zx+0.1f,1.5f*zy,rgb|(uint32_t)(strength*255));
            }
        }
    }

}
static bool before_interface(patch_handle_t instance,void **args,const patch_method_signature_t *sig,void *result) {
    (void)instance;(void)args;(void)sig;(void)result;
    if(own_batch) return false;
    if(main_bool("showSplash",true) || main_bool("gameMenu",true) || main_bool("mapFullscreen",false)) { had_camera=false; return false; }
    void *sb=sprite_batch();
    if(!sb || !begin_called) return false;
    unsigned char *active=patchlib_field_get_pointer(begin_called,sb);
    if(!active) return false;
    bool resume=*active!=0;
    if(resume && (!observed.valid || observed.sb!=sb)) return false;
    int sw=main_int("screenWidth",0),sh=main_int("screenHeight",0);
    float cx,cy,m[16];
    if(sw<=0 || sh<=0 || !main_vec2("screenPosition",&cx,&cy)) return false;
    virtual_view_matrix(m);
    cache_tile_flags();if(!solid_count) return false;
    batch_state saved=observed;
    own_batch=true;
    if(resume) draw_end();
    if(ensure_textures()) {
        draw_begin();g_frames++;
        bool day=main_bool("dayTime",true);
        float t=main_double("time",0)/(day?54000.f:32400.f);
        render_world(sw,sh,cx,cy,m,day,t);
        draw_end();
        if(g_frames==1 || g_frames%600==0) LOG_INFO("v1.1 帧=%d 遮挡格=%d 屏幕=%dx%d",g_frames,grid_count,sw,sh);
    }
    if(resume) restore_batch(&saved);
    own_batch=false;
    return false;
}
static const kernel_mod_info_t g_info={.pkg_id="lzup.lightsandshadows",.version_code=3,.api_version=1,.version="1.1.1"};
static void init_mod(kernel_mod_handle_t *handle) {
    (void)handle;
    LOG_INFO("v1.1.1 init entry");
    if(!patchlib_type_get_type || !patchlib_type_get_field || !patchlib_field_get_pointer ||
       !patchlib_type_get_method_by_param_count || !patchlib_method_get_pointer ||
       !patchlib_method_invoke_args || !patchlib_install_prepost_hook || !patchlib_uninstall_hook ||
       !patchlib_is_valid || !patchlib_array_length || !patchlib_array_copy_to_c ||
       !terraria_texture2d_create) { LOG_ERROR("必需的内核API未注入，停止初始化");return; }
    g_main_type=patchlib_type_get_type("Terraria","Main");
    if(!g_main_type || !draw_init()) return;
    occlusion_init();
    patch_handle_t sb=patchlib_type_get_type("Microsoft.Xna.Framework.Graphics","SpriteBatch");
    begin_called=patchlib_type_get_field(sb,"_beginCalled");
    patch_handle_t ui=patchlib_type_get_method_by_param_count(g_main_type,"DrawInterface",1);
    if(!begin_called || !ui) {LOG_ERROR("必需的世界/UI接口缺失，停止绘制");return;}
    hooks[hook_count++]=patchlib_install_prepost_hook(g_begin9,NULL,observe_begin);
    hooks[hook_count++]=patchlib_install_prepost_hook(g_end0,NULL,observe_end);
    patch_handle_t short_begin=patchlib_type_get_method_by_param_count(sb,"Begin",2);
    if(short_begin) hooks[hook_count++]=patchlib_install_prepost_hook(short_begin,NULL,observe_short_begin);
    hooks[hook_count++]=patchlib_install_prepost_hook(ui,before_interface,NULL);
    for(int i=0;i<hook_count;i++) if(hooks[i]==PATCH_HOOK_INVALID_ID) {
        LOG_ERROR("绘制 hook 安装失败");
        for(int j=0;j<hook_count;j++) if(hooks[j]!=PATCH_HOOK_INVALID_ID) patchlib_uninstall_hook(hooks[j]);
        hook_count=0;return;
    }
    LOG_INFO("v1.1.1 初始化完成：世界阴影位于UI之前，保留原版明暗");
}
static void cleanup_mod(kernel_mod_handle_t *handle) {
    (void)handle;
    for(int i=0;i<hook_count;i++) patchlib_uninstall_hook(hooks[i]);
    hook_count=0;
    if(g_gchandle_free) for(int i=0;i<root_count;i++) g_gchandle_free(texture_roots[i]);
    root_count=0;g_white=NULL;g_soft=NULL;observed.valid=false;had_camera=false;
}
static kernel_mod_info_t *get_info(void) {return (kernel_mod_info_t*)&g_info;}
__attribute__((visibility("default"))) kernel_mod_ops_t *create_kernel_mod(void) {
    static kernel_mod_ops_t ops={.init_mod=init_mod,.cleanup_mod=cleanup_mod,.get_info=get_info};
    return &ops;
}
