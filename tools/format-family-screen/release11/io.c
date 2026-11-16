#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

/* Linux-only diagnostic observer. No HDF5/codec interposition: Python may
 * contain several independently scoped native libraries. Slots 4..15 are
 * unavailable, not zero observations. Counters cover libc calls, not devices.
 * The single-worker campaign serializes these calls; atomics also admit async
 * I/O workers. Path records are protected separately and allocate nothing. */
static _Atomic uint64_t slots[16];
static _Atomic int enabled;
static pthread_once_t once=PTHREAD_ONCE_INIT;
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
static ssize_t (*next_read)(int,void*,size_t);
static ssize_t (*next_pread)(int,void*,size_t,off_t);
static ssize_t (*next_readv)(int,const struct iovec*,int);
static ssize_t (*next_write)(int,const void*,size_t);
static ssize_t (*next_pwrite)(int,const void*,size_t,off_t);
static ssize_t (*next_writev)(int,const struct iovec*,int);
static struct {char path[1024];uint64_t calls,bytes;} chunks[64];
static size_t n_chunks;
static uint64_t omitted_paths;

static void resolve(void) {
    next_read=dlsym(RTLD_NEXT,"read");next_pread=dlsym(RTLD_NEXT,"pread");
    next_readv=dlsym(RTLD_NEXT,"readv");next_write=dlsym(RTLD_NEXT,"write");
    next_pwrite=dlsym(RTLD_NEXT,"pwrite");next_writev=dlsym(RTLD_NEXT,"writev");
}
static void record(int fd,ssize_t n,int writing) {
    if(!atomic_load_explicit(&enabled,memory_order_relaxed))return;
    atomic_fetch_add(&slots[writing?2:0],1);
    if(n>0)atomic_fetch_add(&slots[writing?3:1],(uint64_t)n);
    if(writing || n<=0)return;
    char link[64],path[4096];snprintf(link,sizeof(link),"/proc/self/fd/%d",fd);
    ssize_t size=readlink(link,path,sizeof(path)-1);if(size<0)return;path[size]=0;
    const char* chunk=strstr(path,"/images/image/s0/c/");if(!chunk)return;
    pthread_mutex_lock(&mutex);
    size_t i=0;for(;i<n_chunks;++i)if(!strcmp(chunks[i].path,chunk))break;
    if(i==n_chunks) {
        if(n_chunks==64 || strlen(chunk)>=sizeof(chunks[i].path)) {++omitted_paths;pthread_mutex_unlock(&mutex);return;}
        strcpy(chunks[i].path,chunk);++n_chunks;
    }
    ++chunks[i].calls;chunks[i].bytes+=(uint64_t)n;
    pthread_mutex_unlock(&mutex);
}
void v1_io_begin(void) {
    pthread_once(&once,resolve);for(int i=0;i<16;++i)atomic_store(&slots[i],0);
    n_chunks=0;omitted_paths=0;memset(chunks,0,sizeof(chunks));atomic_store(&enabled,1);
}
void v1_io_end(uint64_t* out) {
    atomic_store(&enabled,0);for(int i=0;i<16;++i)out[i]=atomic_load(&slots[i]);
    fprintf(stderr,"release11_chunk_reads: {\"omitted_paths\":%llu,\"chunks\":[",(unsigned long long)omitted_paths);
    for(size_t i=0;i<n_chunks;++i)fprintf(stderr,"%s{\"path\":\"%s\",\"calls\":%llu,\"bytes\":%llu}",
        i?",":"",chunks[i].path,(unsigned long long)chunks[i].calls,(unsigned long long)chunks[i].bytes);
    fprintf(stderr,"]}\n");
}
ssize_t read(int fd,void* p,size_t n) {pthread_once(&once,resolve);ssize_t r=next_read?next_read(fd,p,n):syscall(SYS_read,fd,p,n);record(fd,r,0);return r;}
ssize_t pread(int fd,void* p,size_t n,off_t at) {pthread_once(&once,resolve);ssize_t r=next_pread?next_pread(fd,p,n,at):syscall(SYS_pread64,fd,p,n,at);record(fd,r,0);return r;}
ssize_t pread64(int fd,void* p,size_t n,off64_t at) {return pread(fd,p,n,(off_t)at);}
ssize_t readv(int fd,const struct iovec* v,int n) {pthread_once(&once,resolve);ssize_t r=next_readv?next_readv(fd,v,n):syscall(SYS_readv,fd,v,n);record(fd,r,0);return r;}
ssize_t write(int fd,const void* p,size_t n) {pthread_once(&once,resolve);ssize_t r=next_write?next_write(fd,p,n):syscall(SYS_write,fd,p,n);record(fd,r,1);return r;}
ssize_t pwrite(int fd,const void* p,size_t n,off_t at) {pthread_once(&once,resolve);ssize_t r=next_pwrite?next_pwrite(fd,p,n,at):syscall(SYS_pwrite64,fd,p,n,at);record(fd,r,1);return r;}
ssize_t pwrite64(int fd,const void* p,size_t n,off64_t at) {return pwrite(fd,p,n,(off_t)at);}
ssize_t writev(int fd,const struct iovec* v,int n) {pthread_once(&once,resolve);ssize_t r=next_writev?next_writev(fd,v,n):syscall(SYS_writev,fd,v,n);record(fd,r,1);return r;}
