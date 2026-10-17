#ifndef LIGHTING_MODEL_H
#define LIGHTING_MODEL_H
#include <math.h>
/* Pure calculations are shared by the renderer and host regression checks. */
static float clamp01(float x) { return fminf(1.f, fmaxf(0.f, x)); }
static float smooth01(float x) { x=clamp01(x); return x*x*(3.f-2.f*x); }
static int tile_active(unsigned short h) { return (h & 32u) && !(h & 64u); }
static float tile_surface(unsigned short h, float u) {
    int slope=(h>>12)&7;
    if(slope==1) return u;
    if(slope==2) return 1.f-u;
    if(slope==3 || slope==4) return 0.f;
    return (h&1024u)?0.5f:0.f;
}
static float ray_origin_y(int y,unsigned short h,int solid) {
    return solid?y+tile_surface(h,0.5f)-0.03f:y+0.5f;
}
#endif
