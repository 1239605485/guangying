/* Exercise callback argument indirection and exact Begin overload restoration. */
#include <assert.h>
#include "../main.c"
static bool restored_long,restored_short;
static void long_begin(void *sb,int sort,void *blend,void *sampler,void *depth,
                       void *raster,void *effect,nullable_mat_t *m,bool deferred) {
    assert(sb==(void*)100 && sort==2 && blend==(void*)101 && sampler==(void*)102);
    assert(depth==(void*)103 && raster==(void*)104 && effect==(void*)105);
    assert(m->has==1 && m->m[0]==2.f && m->m[12]==123.f && deferred);
    restored_long=true;
}
static void short_begin(void *sb,int sort,bool deferred) {
    assert(sb==(void*)100 && sort==2 && deferred);restored_short=true;
}
int main(void) {
    int sort=2;bool deferred=true;
    void *objects[]={(void*)101,(void*)102,(void*)103,(void*)104,(void*)105};
    nullable_mat_t matrix={0};matrix.has=1;matrix.m[0]=2.f;matrix.m[12]=123.f;
    nullable_mat_t *matrix_pointer=&matrix;
    void *args[]={&sort,&objects[0],&objects[1],&objects[2],&objects[3],&objects[4],&matrix_pointer,&deferred};
    observe_begin((void*)100,args,NULL,NULL);
    assert(observed.valid && !observed.short_form);
    assert(!memcmp(&matrix,&observed.matrix,sizeof(matrix)));
    nat_begin=(void*)long_begin;restore_batch(&observed);assert(restored_long);
    void *short_args[]={&sort,&deferred};
    observe_short_begin((void*)100,short_args,NULL,NULL);
    assert(observed.valid && observed.short_form);
    native_begin_short=(void*)short_begin;restore_batch(&observed);assert(restored_short);
    own_batch=true;observe_end((void*)100,NULL,NULL,NULL);assert(observed.valid);
    own_batch=false;observe_end((void*)100,NULL,NULL,NULL);assert(!observed.valid);
    return 0;
}
