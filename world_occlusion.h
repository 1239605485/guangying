#ifndef WORLD_OCCLUSION_H
#define WORLD_OCCLUSION_H
#include "tefkernel/patchlib/struct/array.h"
#include "lighting_model.h"
#define TILE_PX 16
static int *tile_max_x, *tile_max_y;
static uint32_t **tile_lookup;
static uint16_t **tile_types, **tile_headers;
static patch_handle_t solid_field, top_field;
static unsigned char solid_cache[65536], top_cache[65536];
static void *last_solid, *last_top;
static size_t solid_count;
static void cache_tile_flags(void) {
    void **s=solid_field?patchlib_field_get_pointer(solid_field,NULL):NULL;
    void **t=top_field?patchlib_field_get_pointer(top_field,NULL):NULL;
    if(!s || !t || !*s || !*t) { solid_count=0; return; }
    if(solid_count && *s==last_solid && *t==last_top) return;
    size_t n=patchlib_array_length(*s), m=patchlib_array_length(*t);
    if(!n || n>sizeof(solid_cache) || m!=n) { solid_count=0; return; }
    /* TEFKernel 1.1.1 registers array_at/length, but NOT copy_to_c.
     * Read once when the arrays change; no unsupported bulk API dependency. */
    for(size_t i=0;i<n;i++) {
        if(!patchlib_array_at(*s,i,&solid_cache[i]) ||
           !patchlib_array_at(*t,i,&top_cache[i])) { solid_count=0; return; }
    }
    solid_count=n; last_solid=*s; last_top=*t;
}
/* -1 means unavailable, distinct from dirt (type zero) and air. */
static int blocking_tile(int x,int y,unsigned short *header) {
    if(!solid_count || !tile_max_x || !tile_max_y || !tile_lookup ||
       !tile_types || !tile_headers || !*tile_lookup || !*tile_types || !*tile_headers) return -1;
    int nx=*tile_max_x,ny=*tile_max_y;
    if(nx<=0 || ny<=0 || x<0 || y<0 || x>=nx || y>=ny) return -1;
    uint32_t i=(*tile_lookup)[(size_t)y*nx+x];
    if(i==UINT32_MAX) return 0;
    unsigned short h=(*tile_headers)[i], type=(*tile_types)[i];
    if(header) *header=h;
    if(!tile_active(h) || type>=solid_count) return 0;
    return solid_cache[type] && !top_cache[type];
}
/* Test a ray against the actual half brick / slope cross section. */
static int ray_blocked(float x,float y) {
    int tx=(int)floorf(x),ty=(int)floorf(y); unsigned short h=0;
    int b=blocking_tile(tx,ty,&h);
    if(b<=0) return b;
    float u=x-tx,v=y-ty;int slope=(h>>12)&7;
    if(slope==3) return v<=1.f-u;
    if(slope==4) return v<=u;
    return v>=tile_surface(h,u);
}
static void occlusion_init(void) {
    patch_handle_t td=patchlib_type_get_type("Terraria","TileData");
    const char *names[]={"maxTilesX","maxTilesY"};
    int **slots[]={&tile_max_x,&tile_max_y};
    for(int i=0;i<2;i++) {
        patch_handle_t f=patchlib_type_get_field(g_main_type,names[i]);
        *slots[i]=f?patchlib_field_get_pointer(f,NULL):NULL;
    }
    const char *pn[]={"TileLookup","TileType","TileSHeader"};
    void *out[3]={0};
    for(int i=0;i<3;i++) {
        patch_handle_t f=td?patchlib_type_get_field(td,pn[i]):NULL;
        out[i]=f?patchlib_field_get_pointer(f,NULL):NULL;
    }
    tile_lookup=out[0]; tile_types=out[1]; tile_headers=out[2];
    solid_field=patchlib_type_get_field(g_main_type,"tileSolid");
    top_field=patchlib_type_get_field(g_main_type,"tileSolidTop");
    LOG_INFO("遮挡接口 lookup=%p type=%p header=%p",out[0],out[1],out[2]);
}
#endif
