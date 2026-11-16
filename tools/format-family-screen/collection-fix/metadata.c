#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdatomic.h>
static _Atomic uint64_t n[7];
static int64_t (*op)(int64_t,const char*,int64_t),(*gp)(int64_t,const char*,int64_t),(*ty)(int64_t),(*sp)(int64_t),(*pl)(int64_t); static int (*ar)(int64_t,int64_t,void*);
void metadata_init(void){op=dlsym(RTLD_NEXT,"H5Oopen");gp=dlsym(RTLD_NEXT,"H5Gopen2");ty=dlsym(RTLD_NEXT,"H5Dget_type");sp=dlsym(RTLD_NEXT,"H5Dget_space");pl=dlsym(RTLD_NEXT,"H5Dget_create_plist");ar=dlsym(RTLD_NEXT,"H5Aread");}
void metadata_snapshot(uint64_t* x){for(int i=0;i<7;i++)x[i]=atomic_load(&n[i]);}
int64_t H5Oopen(int64_t a,const char*b,int64_t c){if(!op)metadata_init();atomic_fetch_add(&n[0],1);return op(a,b,c);}
int64_t H5Gopen2(int64_t a,const char*b,int64_t c){if(!gp)metadata_init();atomic_fetch_add(&n[5],1);return gp(a,b,c);}
int64_t H5Dget_type(int64_t a){if(!ty)metadata_init();atomic_fetch_add(&n[1],1);return ty(a);}
int64_t H5Dget_space(int64_t a){if(!sp)metadata_init();atomic_fetch_add(&n[2],1);return sp(a);}
int64_t H5Dget_create_plist(int64_t a){if(!pl)metadata_init();atomic_fetch_add(&n[3],1);return pl(a);}
int H5Aread(int64_t a,int64_t b,void*c){if(!ar)metadata_init();atomic_fetch_add(&n[4],1);return ar(a,b,c);}

static int (*read_next)(int64_t,int64_t,int64_t,int64_t,int64_t,void*);
static int (*type_class)(int64_t);
int H5Dread(int64_t a,int64_t b,int64_t c,int64_t d,int64_t e,void *f) {
    if (!read_next) read_next=dlsym(RTLD_NEXT,"H5Dread");
    if (!type_class) type_class=dlsym(RTLD_NEXT,"H5Tget_class");
    if (type_class && type_class(b)==3) atomic_fetch_add(&n[6],1);
    return read_next(a,b,c,d,e,f);
}
