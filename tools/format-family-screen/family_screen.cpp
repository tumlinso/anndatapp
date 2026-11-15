#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <dlfcn.h>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

import anndatapp.h5ad;
import anndatapp.mudata;
import anndatapp.treedata;
import anndatapp.dataset;
import anndatapp.spatialdata;

using namespace anndatapp;
using Clock = std::chrono::steady_clock;
using Time = Clock::time_point;

namespace {
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
long long ns(Time a, Time b) { return std::chrono::duration_cast<std::chrono::nanoseconds>(b-a).count(); }
std::vector<std::ptrdiff_t> positions() {
    std::vector<std::ptrdiff_t> out;
    for (int i=0; i<256; i+=4) out.push_back(i);
    return out;
}
struct Observer {
    using Begin=void(*)(); using End=void(*)(std::uint64_t*);
    Begin io_begin=nullptr, alloc_begin=nullptr; End io_end=nullptr, alloc_end=nullptr;
    std::uint64_t io[16]{}, alloc[2]{}; bool enabled=false;
    explicit Observer(bool diagnostic):enabled(diagnostic) {
        if (!enabled) return;
        io_begin=reinterpret_cast<Begin>(dlsym(RTLD_DEFAULT,"v1_io_begin"));
        io_end=reinterpret_cast<End>(dlsym(RTLD_DEFAULT,"v1_io_end"));
        alloc_begin=reinterpret_cast<Begin>(dlsym(RTLD_DEFAULT,"v1_alloc_begin"));
        alloc_end=reinterpret_cast<End>(dlsym(RTLD_DEFAULT,"v1_alloc_end"));
        require(io_begin && io_end && alloc_begin && alloc_end,"--diagnostic requires v1 I/O and allocation interposers");
    }
    void start() { if (enabled) { io_begin(); alloc_begin(); } }
    void stop() { if (enabled) { alloc_end(alloc); io_end(io); } }
};
Observer* observer=nullptr;
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
std::uint64_t integer(const std::vector<std::uint64_t>& b, DType type, std::size_t i) {
    switch(type) {
    case DType::int32: {auto n=cell<std::int32_t>(b,i); require(n>=0,"negative sparse index"); return n;}
    case DType::uint32: return cell<std::uint32_t>(b,i);
    case DType::int64: {auto n=cell<std::int64_t>(b,i); require(n>=0,"negative sparse index"); return n;}
    case DType::uint64: return cell<std::uint64_t>(b,i);
    default: throw std::runtime_error("unexpected sparse integer dtype");
    }
}
void verify(const Buffers& b,const ReadRequirements& r) {
    require(r.layout==Layout::csr && r.dtype==DType::float32,"expected float32 CSR");
    require(r.shape==std::vector<std::size_t>{64,64} && r.nnz==512,"incorrect shape or nnz");
    for(std::size_t i=0;i<64;++i) {
        require(integer(b.offsets,r.offset_dtype,i)==8*i,"incorrect row offset");
        for(std::size_t k=0;k<8;++k) {
            const auto j=8*i+k;
            require(integer(b.indices,r.index_dtype,j)==8*k,"incorrect column index");
            require(cell<float>(b.values,j)==static_cast<float>(1+(4*i+3*k)%29),"incorrect value");
        }
    }
    require(integer(b.offsets,r.offset_dtype,64)==512,"incorrect final offset");
}
const char* integer_name(DType t) {
    switch (t) {
    case DType::int32: return "int32"; case DType::uint32: return "uint32";
    case DType::int64: return "int64"; case DType::uint64: return "uint64";
    default: return "other";
    }
}
void report(std::string_view family, const ReadRequirements& req, Time start, Time opened, Time ready, Time finished) {
    // Hash is the independently computed formula hash, emitted only after every cell,
    // index, and offset has been checked against that formula.
    std::cout << "{\"case\":\"" << family << "\",\"selector\":\"rows_and_columns_0_4_252\","
        << "\"shape\":[64,64],\"nnz\":512,\"value_dtype\":\"float32\","
        << "\"canonical_index_dtype\":\"uint32\",\"canonical_offset_dtype\":\"uint64\",\"actual_index_dtype\":\"" << integer_name(req.index_dtype)
        << "\",\"actual_offset_dtype\":\"" << integer_name(req.offset_dtype) << "\","
        << "\"sha256\":\"a8da79bc30f9c6f829175701edb00016bae0650b9cc7083b05e9248466dabb15\","
        << "\"hash_source\":\"formula_after_cellwise_verification\",\"golden\":\"pass\",\"output\":\"eager_csr_caller_buffer\","
        << "\"open_ns\":" << ns(start,opened) << ",\"prepare_and_allocate_ns\":" << ns(opened,ready)
        << ",\"operation_ns\":" << ns(ready,finished) << ",\"end_to_end_ns\":" << ns(start,finished);
    if (observer->enabled) std::cout << ",\"diagnostic\":{\"scope\":\"open_through_transfer\",\"intercepted_read_calls\":" << observer->io[0]
        << ",\"intercepted_read_bytes\":" << observer->io[1] << ",\"intercepted_write_calls\":" << observer->io[2]
        << ",\"intercepted_write_bytes\":" << observer->io[3] << ",\"h5dread_calls\":" << observer->io[14]
        << ",\"h5dwrite_calls\":" << observer->io[15] << ",\"cpp_new_calls\":" << observer->alloc[0]
        << ",\"cpp_new_bytes\":" << observer->alloc[1] << "}";
    std::cout << "}\n";
}
template<class Factory> void read_array(std::string_view name, Time start, Time opened, Factory&& source) {
    auto pos=positions();
    auto array=source(pos);
    auto prepared=array.prepare_read();
    auto req=prepared.requirements();
    Buffers b(req);
    const auto ready=Clock::now();
    prepared.read_into(b.dst);
    const auto finished=Clock::now();
    observer->stop();
    verify(b,req);
    report(name,req,start,opened,ready,finished);
}
}
int main(int argc,char** argv) {
    try {
        require(argc==3 || (argc==4 && std::string_view(argv[3])=="--diagnostic"),"usage: family-screen <anndata|mudata|treedata|dataset|spatialdata|mudata-map|treedata-tree|spatialdata-raster|spatialdata-points> <path> [--diagnostic]");
        Observer metrics(argc==4); observer=&metrics; metrics.start();
        const std::string_view name(argv[1]);
        const auto start=Clock::now();
        if(name=="mudata-map") {
            auto data=open_h5mu(argv[2]); const auto opened=Clock::now();
            auto p=positions(); auto selected=data.slice(indices(p),indices(p));
            const auto sliced=Clock::now();
            auto obs=selected.obs_map("rna"); auto var=selected.var_map("rna");
            const auto mapped=Clock::now();
            auto modality=selected.modality("rna"); const auto finished=Clock::now();
            observer->stop();
            require(obs.size==64 && var.size==64 && modality.n_obs()==64 && modality.n_vars()==64,"incorrect selected maps or modality dimensions");
            for(std::size_t i=0;i<64;++i) require(obs.data[i]==i+1 && var.data[i]==i+1,"incorrect projected map");
            std::cout << "{\"case\":\"mudata-map\",\"golden\":\"pass\",\"output\":\"retained_maps_and_lazy_modality\",\"open_ns\":" << ns(start,opened)
                << ",\"selection_ns\":" << ns(opened,sliced) << ",\"map_acquire_ns\":" << ns(sliced,mapped)
                << ",\"modality_acquire_ns\":" << ns(mapped,finished) << ",\"end_to_end_ns\":" << ns(start,finished);
            if(observer->enabled) std::cout << ",\"diagnostic\":{\"scope\":\"open_through_acquisition\",\"intercepted_read_calls\":" << observer->io[0]
                << ",\"intercepted_read_bytes\":" << observer->io[1] << ",\"h5dread_calls\":" << observer->io[14]
                << ",\"cpp_new_calls\":" << observer->alloc[0] << ",\"cpp_new_bytes\":" << observer->alloc[1] << "}";
            std::cout << "}\n";
        } else if(name=="treedata-tree") {
            auto data=open_h5td(argv[2]); const auto opened=Clock::now();
            auto names=data.obs_tree_names(); const auto listed=Clock::now();
            auto full=data.obs_tree("lineage"); const auto acquired=Clock::now();
            auto p=positions(); auto selected=data.read(indices(p),all); const auto finished=Clock::now();
            observer->stop();
            require(names.size()==2 && std::find(names.begin(),names.end(),"lineage")!=names.end() &&
                    std::find(names.begin(),names.end(),"unused")!=names.end() &&
                    full.n_nodes()==257 && full.n_edges()==256,"incorrect tree discovery");
            auto tree=selected.obs_tree("lineage");
            require(tree.n_nodes()==65 && tree.n_edges()==64,"incorrect selected tree shape");
            auto ids=tree.node_ids(); auto edges=tree.edges().dense<std::uint64_t>();
            require(ids.string(0)=="root" && tree.node_attributes(0).at("rank").signed_integer()==-1,"incorrect root");
            for(std::size_t i=0;i<64;++i) {
                char id[32]; std::snprintf(id,sizeof(id),"cell-%03zu",4*i);
                require(ids.string(i+1)==id,"incorrect selected node");
                require(edges.data()[2*i]==0 && edges.data()[2*i+1]==i+1,"incorrect selected edge");
                require(tree.node_attributes(i+1).at("rank").signed_integer()==static_cast<std::int64_t>(4*i),"incorrect rank");
                require(tree.edge_attributes(i).at("length").floating()==1.0+double(4*i)/256.0,"incorrect edge length");
            }
            std::cout << "{\"case\":\"treedata-tree\",\"golden\":\"pass\",\"output\":\"named_tree_then_eager_selected_treedata_all_trees\",\"open_ns\":" << ns(start,opened)
                << ",\"names_ns\":" << ns(opened,listed) << ",\"named_tree_ns\":" << ns(listed,acquired)
                << ",\"selected_treedata_ns\":" << ns(acquired,finished) << ",\"end_to_end_ns\":" << ns(start,finished);
            if(observer->enabled) std::cout << ",\"diagnostic\":{\"scope\":\"open_through_selected_read\",\"intercepted_read_calls\":" << observer->io[0]
                << ",\"intercepted_read_bytes\":" << observer->io[1] << ",\"h5dread_calls\":" << observer->io[14]
                << ",\"cpp_new_calls\":" << observer->alloc[0] << ",\"cpp_new_bytes\":" << observer->alloc[1] << "}";
            std::cout << "}\n";
        } else if(name=="spatialdata-raster") {
            auto data=open_spatialdata(argv[2]); const auto opened=Clock::now();
            auto array=data.raster("image").select({all,range{16,80,2},range{32,96,2}});
            std::vector<std::uint16_t> values(1024); const auto ready=Clock::now();
            array.read_into(DenseBuffer::from(values.data(),{1,32,32})); const auto finished=Clock::now();
            observer->stop();
            for(std::size_t y=0;y<32;++y) for(std::size_t x=0;x<32;++x)
                require(values[y*32+x]==256*(16+2*y)+32+2*x,"incorrect raster value");
            std::cout << "{\"case\":\"spatialdata-raster\",\"golden\":\"pass\",\"output\":\"eager_dense_caller_buffer\",\"shape\":[1,32,32],\"open_ns\":" << ns(start,opened)
                << ",\"prepare_and_allocate_ns\":" << ns(opened,ready) << ",\"operation_ns\":" << ns(ready,finished)
                << ",\"end_to_end_ns\":" << ns(start,finished);
            if(observer->enabled) std::cout << ",\"diagnostic\":{\"scope\":\"open_through_transfer\",\"intercepted_read_calls\":" << observer->io[0]
                << ",\"intercepted_read_bytes\":" << observer->io[1] << ",\"cpp_new_calls\":" << observer->alloc[0] << ",\"cpp_new_bytes\":" << observer->alloc[1] << "}";
            std::cout << "}\n";
        } else if(name=="spatialdata-points") {
            auto data=open_spatialdata(argv[2]); const auto opened=Clock::now();
            auto points=data.read_points("points",16,8); const auto finished=Clock::now();
            observer->stop();
            auto view=points.view(); require(view.size()==8,"incorrect points batch size");
            auto x=view.values().column("x").values().dense<double>();
            auto y=view.values().column("y").values().dense<double>();
            auto id=view.identifiers().values().dense<std::int64_t>();
            auto instance=view.values().column("spot_id").values().dense<std::int64_t>();
            require(view.instance_key()=="spot_id","incorrect point instance key");
            for(std::size_t i=0;i<8;++i) {
                require(x.data()[i]==double(16+i)+.25 && y.data()[i]==2.0*(16+i)+.5,"incorrect point coordinate");
                require(id.data()[i]==static_cast<std::int64_t>(16+i),"incorrect point row index");
                require(instance.data()[i]==static_cast<std::int64_t>(1016+i),"incorrect point instance value");
            }
            std::cout << "{\"case\":\"spatialdata-points\",\"golden\":\"pass\",\"output\":\"eager_bounded_points_batch\",\"first\":16,\"count\":8,\"open_ns\":" << ns(start,opened)
                << ",\"operation_ns\":" << ns(opened,finished) << ",\"end_to_end_ns\":" << ns(start,finished);
            if(observer->enabled) std::cout << ",\"diagnostic\":{\"scope\":\"open_through_batch\",\"intercepted_read_calls\":" << observer->io[0]
                << ",\"intercepted_read_bytes\":" << observer->io[1] << ",\"cpp_new_calls\":" << observer->alloc[0] << ",\"cpp_new_bytes\":" << observer->alloc[1] << "}";
            std::cout << "}\n";
        } else if(name=="anndata") {
            auto data=open_h5ad(argv[2]); const auto opened=Clock::now();
            read_array(name,start,opened,[&](const auto& p){return data.slice(indices(p),indices(p)).x();});
        } else if(name=="mudata") {
            auto data=open_h5mu(argv[2]); const auto opened=Clock::now();
            read_array(name,start,opened,[&](const auto& p){return data.slice(indices(p),indices(p)).modality("rna").x();});
        } else if(name=="treedata") {
            auto data=open_h5td(argv[2]); const auto opened=Clock::now();
            read_array(name,start,opened,[&](const auto& p){return data.data().slice(indices(p),indices(p)).x();});
        } else if(name=="spatialdata") {
            auto data=open_spatialdata(argv[2]); const auto opened=Clock::now();
            read_array(name,start,opened,[&](const auto& p){return data.table("table").slice(indices(p),indices(p)).x();});
        } else if(name=="dataset") {
            auto data=open_h5ads(argv[2]); const auto opened=Clock::now();
            auto p=positions(); auto view=data.slice(indices(p),indices(p));
            auto req=view.requirements(); Buffers b(req); const auto ready=Clock::now();
            view.read_x_into(b.dst); const auto finished=Clock::now();
            observer->stop();
            verify(b,req); report(name,req,start,opened,ready,finished);
        } else throw std::runtime_error("unknown case");
    } catch(const std::exception& e) { std::cerr << "family-screen: " << e.what() << '\n'; return 1; }
}
