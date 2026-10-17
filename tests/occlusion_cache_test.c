#include <assert.h>
#include <string.h>
#include <stdint.h>
#include "../tefkernel/patchlib/field.h"
static patch_handle_t g_main_type=(void*)100;
#define LOG_INFO(...) ((void)0)
#include "../world_occlusion.h"

typedef struct {size_t count; unsigned char bytes[2];} fake_array;
static fake_array solid={2,{1,1}},tops={2,{0,0}},platforms={2,{1,0}};
static void *solid_object=&solid,*top_object=&tops;
static int nx=2,ny=2,reads;
static uint32_t lookup_data[]={0,1,2,3},*lookup_ptr=lookup_data;
static uint16_t types_data[]={0,1,0,0},*types_ptr=types_data;
static uint16_t headers_data[]={32,0,96,32|1024},*headers_ptr=headers_data;
static patch_handle_t get_type(const char *ns,const char *name) {
    assert(!strcmp(ns,"Terraria") && !strcmp(name,"TileData"));return (void*)101;
}
static patch_handle_t get_field(patch_handle_t type,const char *name) {
    (void)type;
    const char *names[]={"tileSolid","tileSolidTop","maxTilesX","maxTilesY","TileLookup","TileType","TileSHeader"};
    for(int i=0;i<7;i++) if(!strcmp(name,names[i])) return (void*)(uintptr_t)(i+1);
    return NULL;
}
static void *field_pointer(patch_handle_t field,void *instance) {
    (void)instance;
    void *slots[]={&solid_object,&top_object,&nx,&ny,&lookup_ptr,&types_ptr,&headers_ptr};
    uintptr_t i=(uintptr_t)field;return i>=1 && i<=7?slots[i-1]:NULL;
}
static size_t length(patch_handle_t array) {return ((fake_array*)array)->count;}
static bool at(patch_handle_t array,size_t i,void *out) {
    fake_array *a=array;if(i>=a->count)return false;*(unsigned char*)out=a->bytes[i];reads++;return true;
}
int main(void) {
    patchlib_type_get_type=get_type;patchlib_type_get_field=get_field;
    patchlib_field_get_pointer=field_pointer;patchlib_array_length=length;patchlib_array_at=at;
    /* Reproduce the deployed kernel: bulk-copy API is present in headers only. */
    assert(!patchlib_array_copy_to_c);
    occlusion_init();cache_tile_flags();assert(solid_count==2 && reads==4);
    cache_tile_flags();assert(reads==4); /* read once, not every frame */
    assert(blocking_tile(0,0,NULL)==1); /* type-zero dirt is solid */
    assert(blocking_tile(1,0,NULL)==0);assert(blocking_tile(0,1,NULL)==0);
    assert(ray_blocked(1.5f,1.25f)==0);assert(ray_blocked(1.5f,1.75f)==1);
    solid_object=NULL;cache_tile_flags();assert(solid_count==0);
    solid_object=&solid;cache_tile_flags();assert(solid_count==2 && reads==8);
    top_object=&platforms;cache_tile_flags();assert(blocking_tile(0,0,NULL)==0);
    assert(smooth01(0.5f)==0.5f);assert(ray_origin_y(10,32,1)<10);
    return 0;
}
