/* Exercise the real world callback: active game batches must not be restarted. */
#include <assert.h>
#include "../main.c"
static int sw=64,sh=64,nx=32,ny=32,draws,begins,ends;
static bool splash,menu,map,day=true,active=true;
static double clock_value=27000,surface=100;
static void *sb_object=(void*)100,*solid_array=(void*)101,*top_array=(void*)102;
static uint32_t lookup_data[1024],*lookup_pointer=lookup_data;
static uint16_t type_data[]={0},header_data[]={32},*type_pointer=type_data,*header_pointer=header_data;
static patch_handle_t field(patch_handle_t type,const char *name) {
    (void)type;
    const char *names[]={"screenWidth","screenHeight","showSplash","gameMenu","mapFullscreen","dayTime","time","worldSurface","spriteBatch"};
    for(int i=0;i<9;i++)if(!strcmp(name,names[i]))return (void*)(uintptr_t)(i+1);
    return NULL;
}
static void *pointer(patch_handle_t field,void *instance) {
    (void)instance;
    void *slots[]={&sw,&sh,&splash,&menu,&map,&day,&clock_value,&surface,&sb_object,&active,&solid_array,&top_array};
    uintptr_t i=(uintptr_t)field;return i && i<=12?slots[i-1]:NULL;
}
static patch_handle_t method(patch_handle_t type,const char *name,int count) {
    (void)type;(void)count;return !strcmp(name,"get_screenPosition")?(void*)20:NULL;
}
static bool valid(patch_handle_t handle) {return handle!=NULL;}
static bool invoke(patch_handle_t method,patch_handle_t instance,void *result,void **args) {
    (void)instance;(void)args;assert(method==(void*)20);float p[]={16,16};memcpy(result,p,sizeof(p));return true;
}
static size_t length(patch_handle_t array) {(void)array;return 1;}
static bool at(patch_handle_t array,size_t index,void *out) {
    assert(index==0);*(unsigned char*)out=array==(void*)101;return true;
}
static void begin(void *sb,int sort,bool deferred) {
    assert(sb==sb_object && sort==0 && deferred && !active);active=true;begins++;
}
static void end(void *sb) {assert(sb==sb_object && active);active=false;ends++;}
static void draw(void *sb,void *texture,v2_t pos,rect_t *src,uint32_t color,float rot,v2_t origin,v2_t scale,int effects,float depth) {
    (void)pos;(void)color;(void)rot;(void)origin;(void)effects;(void)depth;
    assert(sb==sb_object && texture==(void*)8 && active && src->w==1 && scale.x>0);draws++;
}
/* No actual texture upload occurs: this test injects an existing retained pixel. */
void *xdl_open(const char *name,int flags) {(void)name;(void)flags;return NULL;}
void *xdl_sym(void *handle,const char *name,size_t *size) {(void)handle;(void)name;(void)size;return NULL;}
int main(void) {
    patchlib_type_get_field=field;patchlib_field_get_pointer=pointer;
    patchlib_type_get_method_by_param_count=method;patchlib_is_valid=valid;
    patchlib_method_invoke_args=invoke;patchlib_array_length=length;patchlib_array_at=at;
    g_main_type=(void*)1;g_sb_field=(void*)9;begin_called=(void*)10;
    solid_field=(void*)11;top_field=(void*)12;
    tile_max_x=&nx;tile_max_y=&ny;tile_lookup=&lookup_pointer;tile_types=&type_pointer;tile_headers=&header_pointer;
    for(int i=0;i<1024;i++)lookup_data[i]=UINT32_MAX;
    for(int x=0;x<32;x++)lookup_data[4*32+x]=0;
    g_white=(void*)8;nat_draw=(void*)draw;native_begin_short=(void*)begin;native_end=(void*)end;
    before_interface(NULL,NULL,NULL,NULL);
    assert(draws>0 && begins==0 && ends==0 && active); /* borrow without mutation */
    int old_draws=draws;active=false;
    before_interface(NULL,NULL,NULL,NULL);
    assert(draws>old_draws && begins==1 && ends==1 && !active); /* own exactly one pair */
    old_draws=draws;menu=true;
    before_interface(NULL,NULL,NULL,NULL);
    assert(draws==old_draws && begins==1 && ends==1);
    return 0;
}
