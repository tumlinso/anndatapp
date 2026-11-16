#include <cstdint>
#include <ctime>
#include <cstdio>
struct Rec { uint64_t calls, incl[12], excl[12]; }; static Rec rec[12];
struct Frame { int id; uint64_t start[12], child[12]; }; static Frame stack[16]; static int depth,active; static void (*snapshot)(uint64_t*); static uint64_t pieces; static int retained;
static void snap(uint64_t* x) { timespec t; clock_gettime(CLOCK_MONOTONIC,&t); x[0]=uint64_t(t.tv_sec)*1000000000+t.tv_nsec; for(int i=1;i<12;i++)x[i]=0; if(snapshot) snapshot(x+1); }
extern "C" void phase_setup(int on,void (*fn)(uint64_t*)) { active=on; snapshot=fn; }
extern "C" void phase_enter(int id) { if(!active)return; Frame& f=stack[depth++]; f={}; f.id=id; snap(f.start); }
extern "C" void phase_leave(int id) { if(!active)return; uint64_t end[12]; snap(end); Frame& f=stack[--depth]; if(f.id!=id) __builtin_trap(); auto& r=rec[id]; r.calls++; for(int i=0;i<12;i++){auto d=end[i]-f.start[i]; r.incl[i]+=d; r.excl[i]+=d-f.child[i]; if(depth)stack[depth-1].child[i]+=d;} }
extern "C" void phase_piece(unsigned long n,int r) { if(active){pieces=n; retained=r;} }
extern "C" void phase_report() { const char* names[]={"prepare","validation","var","obs","first_metadata","selected_x","component_prepare","descriptor","sparse_sizing","retain","numeric_access","h5dread_call"}; const char* metrics[]={"ns","read_calls","read_bytes","h5dread","new_calls","new_bytes","h5oopen","h5dget_type","h5dget_space","h5dget_create_plist","h5aread","h5gopen2"}; printf("\"retained\":%s,\"pieces\":%llu,\"phases\":{",retained?"true":"false",(unsigned long long)pieces); for(int i=0;i<12;i++){printf("%s\"%s\":{\"calls\":%llu,\"inclusive\":{",i?",":"",names[i],(unsigned long long)rec[i].calls);for(int j=0;j<12;j++)printf("%s\"%s\":%llu",j?",":"",metrics[j],(unsigned long long)rec[i].incl[j]);printf("},\"exclusive\":{");for(int j=0;j<12;j++)printf("%s\"%s\":%llu",j?",":"",metrics[j],(unsigned long long)rec[i].excl[j]);printf("}}");}printf("}"); }
