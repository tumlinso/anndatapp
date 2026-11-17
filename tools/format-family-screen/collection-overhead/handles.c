#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdatomic.h>

static _Atomic uint64_t counts[2];
static int64_t (*open_next)(int64_t, const char*, int64_t);
static int (*close_next)(int64_t);
void dataset_handle_snapshot(uint64_t* out) {
    for (int i = 0; i < 2; ++i) out[i] = atomic_load(&counts[i]);
}
int64_t H5Dopen2(int64_t location, const char* name, int64_t access) {
    if (!open_next) open_next = dlsym(RTLD_NEXT, "H5Dopen2");
    atomic_fetch_add(&counts[0], 1);
    return open_next(location, name, access);
}
int H5Dclose(int64_t dataset) {
    if (!close_next) close_next = dlsym(RTLD_NEXT, "H5Dclose");
    atomic_fetch_add(&counts[1], 1);
    return close_next(dataset);
}
