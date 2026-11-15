#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/resource.h>
#include <vector>

import anndatapp.dataset;
using namespace anndatapp;
using Clock = std::chrono::steady_clock;

namespace {
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
std::vector<std::ptrdiff_t> positions(std::size_t extent) {
    const auto n = extent < 64 ? extent : std::size_t{64};
    std::vector<std::ptrdiff_t> out;
    for (std::size_t i=0; i<n; ++i) out.push_back(n == 1 ? (extent-1)/2 : ((extent-1)/(n-1))*i + (((extent-1)%(n-1))*i)/(n-1));
    return out;
}
std::size_t words(std::size_t count, std::size_t width) { return (count*width+7)/8; }
struct Buffers {
    std::vector<std::uint64_t> values, indices, offsets;
    SparseBuffer dst;
    explicit Buffers(const ReadRequirements& r)
        : values(words(r.nnz,dtype_size(r.dtype))), indices(words(r.nnz,dtype_size(r.index_dtype))),
          offsets(words(r.shape.at(0)+1,dtype_size(r.offset_dtype))),
          dst{values.data(),indices.data(),offsets.data(),r.dtype,r.index_dtype,r.offset_dtype,
              r.nnz,r.nnz,r.shape.at(0)+1} {}
};
template<class T> T cell(const std::vector<std::uint64_t>& b,std::size_t i) {
    T x; std::memcpy(&x,reinterpret_cast<const std::byte*>(b.data())+i*sizeof(T),sizeof(T)); return x;
}
void verify(const Buffers& b,const ReadRequirements& r,const std::vector<std::ptrdiff_t>& rows,const std::vector<std::ptrdiff_t>& genes) {
    require(r.layout==Layout::csr && r.dtype==DType::float32 && r.index_dtype==DType::int32 && r.offset_dtype==DType::int64,"unexpected sparse result type");
    require(r.shape==std::vector<std::size_t>{rows.size(),genes.size()},"unexpected result shape");
    std::size_t k=0;
    for(std::size_t i=0;i<rows.size();++i) {
        require(cell<std::int64_t>(b.offsets,i)==static_cast<std::int64_t>(k),"incorrect row offset");
        for(std::size_t col=0;col<genes.size();++col) {
            const auto difference=genes[col]-rows[i]%97;
            if(difference<0 || difference%199 || difference/199>=100) continue;
            require(k<r.nnz,"missing sparse entry");
            const auto v=static_cast<float>(1+(rows[i]+difference/199)%31);
            require(cell<std::int32_t>(b.indices,k)==static_cast<std::int32_t>(col),"incorrect sparse index");
            require(cell<float>(b.values,k)==v,"incorrect sparse value");
            ++k;
        }
    }
    require(k==r.nnz && cell<std::int64_t>(b.offsets,rows.size())==static_cast<std::int64_t>(k),"incorrect sparse nnz/final offset");
}
struct Observer {
    using Begin=void(*)(); using End=void(*)(std::uint64_t*);
    Begin io_begin=nullptr, alloc_begin=nullptr; End io_end=nullptr, alloc_end=nullptr;
    std::uint64_t io[16]{}, alloc[2]{}; bool enabled=false;
    explicit Observer(bool diag):enabled(diag) {
        if (!diag) return;
        io_begin=reinterpret_cast<Begin>(dlsym(RTLD_DEFAULT,"v1_io_begin"));
        io_end=reinterpret_cast<End>(dlsym(RTLD_DEFAULT,"v1_io_end"));
        alloc_begin=reinterpret_cast<Begin>(dlsym(RTLD_DEFAULT,"v1_alloc_begin"));
        alloc_end=reinterpret_cast<End>(dlsym(RTLD_DEFAULT,"v1_alloc_end"));
        require(io_begin && io_end && alloc_begin && alloc_end,"diagnostic requires v1 I/O and allocation interposers");
    }
    void start() { if(enabled) {io_begin();alloc_begin();} }
    void stop() { if(enabled) {alloc_end(alloc);io_end(io);} }
};
long ms_ns(Clock::time_point a,Clock::time_point b) { return std::chrono::duration_cast<std::chrono::nanoseconds>(b-a).count(); }
}
int main(int argc,char** argv) {
    try {
        require(argc==4,"usage: dataset-screen <end-to-end|prepared|preflight> manifest <timing|diagnostic>");
        std::string_view mode(argv[1]), metric(argv[3]);
        require(mode=="end-to-end" || mode=="prepared" || mode=="preflight","invalid case");
        require(metric=="timing" || metric=="diagnostic","invalid observer mode");
#if !DATASET_PREPARED
        require(mode!="preflight","old API has no standalone preflight");
#endif
        Observer observer(metric=="diagnostic");
        rusage before{},after{}; getrusage(RUSAGE_SELF,&before);
        const auto start=Clock::now();
        if(mode=="end-to-end") observer.start();
        auto owner=open_h5ads(argv[2]);
        const auto opened=Clock::now();
        auto rows=positions(owner.n_obs()), genes=positions(owner.n_vars());
        auto view=owner.slice(indices(rows),indices(genes));
#if DATASET_PREPARED
        auto prepared=view.prepare_x();
        auto req=prepared.requirements();
#else
        auto req=view.requirements();
#endif
        Buffers result(req);
        const auto ready=Clock::now();
        if(mode!="end-to-end") observer.start();
        const auto transfer_start=Clock::now();
#if DATASET_PREPARED
        if(mode=="preflight") prepared.preflight(result.dst); else prepared.read_into(result.dst);
#else
        view.read_x_into(result.dst);
#endif
        const auto finish=Clock::now(); observer.stop(); getrusage(RUSAGE_SELF,&after);
        if(mode!="preflight") verify(result,req,rows,genes);
        std::cout << "{\"case\":\"" << mode << "\",\"api\":\"" << (DATASET_PREPARED?"prepared":"legacy")
                  << "\",\"mode\":\"" << metric << "\",\"shape\":[" << rows.size() << ',' << genes.size()
                  << "],\"nnz\":" << req.nnz << ",\"golden\":\"" << (mode=="preflight"?"not-applicable":"pass")
                  << "\",\"open_ns\":" << ms_ns(start,opened) << ",\"prepare_and_allocate_ns\":" << ms_ns(opened,ready)
                  << ",\"setup_ns\":" << ms_ns(start,ready) << ",\"operation_ns\":" << ms_ns(transfer_start,finish)
                  << ",\"end_to_end_ns\":" << ms_ns(start,finish)
                  << ",\"rss_peak_kib\":" << after.ru_maxrss << ",\"minor_faults\":" << after.ru_minflt-before.ru_minflt
                  << ",\"major_faults\":" << after.ru_majflt-before.ru_majflt;
        if(observer.enabled) std::cout << ",\"intercepted_read_calls\":" << observer.io[0] << ",\"intercepted_read_bytes\":" << observer.io[1]
            << ",\"intercepted_write_calls\":" << observer.io[2] << ",\"intercepted_write_bytes\":" << observer.io[3]
            << ",\"h5dread_calls\":" << observer.io[14] << ",\"h5dwrite_calls\":" << observer.io[15]
            << ",\"cpp_new_calls\":" << observer.alloc[0] << ",\"cpp_new_bytes\":" << observer.alloc[1];
        std::cout << "}\n";
    } catch(const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
