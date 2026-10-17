#include <assert.h>
#include "../lighting_model.h"
int main(void) {
 assert(floorf(ray_origin_y(10,32,1)-0.3f)<10); /* flat ground never occludes itself */
 assert(ray_origin_y(10,32|1024,1)>10.4f);
 assert(tile_active(32)); assert(!tile_active(0)); assert(!tile_active(96));
 assert(tile_surface(32|1024,0.5f)==0.5f);
 assert(tile_surface(32|4096,0.25f)==0.25f);
 assert(tile_surface(32|8192,0.25f)==0.75f);
 assert(smooth01(-1)==0);assert(smooth01(2)==1);
 return 0;
}
