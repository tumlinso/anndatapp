#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stddef.h>
#include <zstd.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <string.h>
static void (*begin_next)(void);
static void (*end_next)(uint64_t*);
static size_t (*decode_next)(ZSTD_DStream*,ZSTD_outBuffer*,ZSTD_inBuffer*);
static uint64_t calls,input_bytes,output_bytes,frame_ends;
static int observing;
static ssize_t (*read_next)(int,void*,size_t);
static ssize_t (*pread_next)(int,void*,size_t,off_t);
static ssize_t (*readv_next)(int,const struct iovec*,int);
static uint64_t chunk_calls,chunk_bytes,metadata_calls,metadata_bytes;
static void observe_fd(int fd,ssize_t size) {
    if(!observing) return;
    char fdpath[64],path[4096];
    snprintf(fdpath,sizeof(fdpath),"/proc/self/fd/%d",fd);
    ssize_t n=readlink(fdpath,path,sizeof(path)-1);
    if(n<0) return;
    path[n]=0;
    if(strstr(path,"/images/image/s0/c/0/0/0")) {++chunk_calls;if(size>0)chunk_bytes+=size;}
    if(strstr(path,"/images/image/s0/zarr.json")) {++metadata_calls;if(size>0)metadata_bytes+=size;}
}
ssize_t read(int fd,void* p,size_t n) {
    if(!read_next) read_next=dlsym(RTLD_NEXT,"read");
    ssize_t size=read_next(fd,p,n);observe_fd(fd,size);return size;
}
ssize_t pread(int fd,void* p,size_t n,off_t at) {
    if(!pread_next) pread_next=dlsym(RTLD_NEXT,"pread");
    ssize_t size=pread_next(fd,p,n,at);observe_fd(fd,size);return size;
}
ssize_t readv(int fd,const struct iovec* v,int n) {
    if(!readv_next) readv_next=dlsym(RTLD_NEXT,"readv");
    ssize_t size=readv_next(fd,v,n);observe_fd(fd,size);return size;
}
void v1_io_begin(void) {
    begin_next=dlsym(RTLD_NEXT,"v1_io_begin");
    end_next=dlsym(RTLD_NEXT,"v1_io_end");
    decode_next=dlsym(RTLD_NEXT,"ZSTD_decompressStream");
    calls=input_bytes=output_bytes=frame_ends=0;
    chunk_calls=chunk_bytes=metadata_calls=metadata_bytes=0;
    read_next=dlsym(RTLD_NEXT,"read");
    pread_next=dlsym(RTLD_NEXT,"pread");
    readv_next=dlsym(RTLD_NEXT,"readv");
    begin_next(); observing=1;
}
void v1_io_end(uint64_t *out) {
    observing=0; end_next(out);
    fprintf(stderr,"raster_codec_observer: calls=%llu input_bytes=%llu output_bytes=%llu frame_ends=%llu io_slots=",
            (unsigned long long)calls,(unsigned long long)input_bytes,(unsigned long long)output_bytes,(unsigned long long)frame_ends);
    for(int i=0;i<16;++i) fprintf(stderr,"%s%llu",i?",":"",(unsigned long long)out[i]);
    fprintf(stderr," chunk_read_calls=%llu chunk_read_bytes=%llu level_metadata_read_calls=%llu level_metadata_read_bytes=%llu\n",
            (unsigned long long)chunk_calls,(unsigned long long)chunk_bytes,(unsigned long long)metadata_calls,(unsigned long long)metadata_bytes);
}
size_t ZSTD_decompressStream(ZSTD_DStream* ctx,ZSTD_outBuffer* out,ZSTD_inBuffer* in) {
    if(!decode_next) decode_next=dlsym(RTLD_NEXT,"ZSTD_decompressStream");
    const size_t a=in->pos,b=out->pos;
    size_t result=decode_next(ctx,out,in);
    if(observing) {++calls;input_bytes+=in->pos-a;output_bytes+=out->pos-b;frame_ends+=result==0;}
    return result;
}
