#include "zarr_engine.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

import anndatapp;

using namespace anndatapp;
using anndatapp_zarr::Json;
namespace {
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void check(bool ok, const std::string& message) { if (!ok) throw std::runtime_error(message); }
Json load_json(const std::filesystem::path& p) { std::ifstream in(p); Json j; in >> j; return j; }
void save_json(const std::filesystem::path& p, const Json& j) { std::ofstream out(p); out << j.dump(2); }

struct Fixture {
    std::filesystem::path root;
    std::vector<float> x, cube;
    std::vector<double> weights;
    std::vector<std::int64_t> counts;
    std::array<bool,99> mask{};
};
template<class T> Array array(std::vector<std::size_t> shape, const std::vector<T>& values) {
    auto a = Array::empty<T>(shape); auto* p = a.edit().template dense<T>().data();
    std::copy(values.begin(), values.end(), p); return a;
}
template<class T> std::vector<T> sequence(std::size_t n, T base, T step) {
    std::vector<T> v(n); for (std::size_t i = 0; i < n; ++i) v[i] = base + T(i) * step; return v;
}
void shard_metadata(const std::filesystem::path& root, const std::string& path,
                    const std::vector<std::size_t>& shape,
                    const std::vector<std::size_t>& outer,
                    const std::vector<std::size_t>& inner, Json fill = nullptr) {
    const auto file = root / path / "zarr.json"; auto m = load_json(file);
    m["shape"] = shape; m["chunk_grid"]["configuration"]["chunk_shape"] = outer;
    m["codecs"] = Json::array({Json{{"name","sharding_indexed"},{"configuration",{
        {"chunk_shape",inner},
        {"codecs",Json::array({Json{{"name","bytes"},{"configuration",{{"endian","little"}}}},
                                Json{{"name","zstd"},{"configuration",{{"level",1}}}}})},
        {"index_codecs",Json::array({Json{{"name","bytes"},{"configuration",{{"endian","little"}}}},
                                      Json{{"name","crc32c"}}})},
        {"index_location","end"}}}}});
    if (!fill.is_null()) m["fill_value"] = fill;
    save_json(file,m);
    const auto root_file = root / "zarr.json"; auto r = load_json(root_file);
    if (r.contains("consolidated_metadata")) r["consolidated_metadata"]["metadata"][path] = m;
    save_json(root_file,r);
    std::filesystem::remove_all(root / path / "c");
}
Fixture fixture(const std::filesystem::path& path) {
    std::filesystem::create_directories(path.parent_path());
    Fixture f; f.root = path;
    f.x = sequence<float>(99, 0.5f, 3.0f);
    f.weights = sequence<double>(99, -2.25, 0.125);
    f.counts = sequence<std::int64_t>(99, 100000, 17);
    for (std::size_t i = 0; i < f.mask.size(); ++i) f.mask[i] = (i % 3) == 1;
    f.cube = sequence<float>(5*4*7, -10.0f, 0.75f);

    auto d = AnnData::empty(9,11);
    d.set_x(array<float>({9,11},f.x));
    d.set_uns("weights",Element(array<double>({9,11},f.weights)));
    d.set_uns("counts",Element(array<std::int64_t>({9,11},f.counts)));
    auto mask = Array::empty<bool>({9,11}); std::copy(f.mask.begin(),f.mask.end(),mask.edit().dense<bool>().data());
    d.set_uns("mask",Element(std::move(mask)));
    d.set_uns("cube",Element(array<float>({5,4,7},f.cube)));
    d.set_uns("huge",Element(array<std::int64_t>({1,1},std::vector<std::int64_t>{0})));
    d.set_uns("fill",Element(array<double>({5,7},std::vector<double>(35,0.0))));
    auto scalar = Array::empty<std::int32_t>({}); scalar.edit().dense<std::int32_t>().data()[0] = 37;
    d.set_uns("scalar",Element(std::move(scalar)));
    d.set_uns("empty",Element(Array::empty<double>({0,3})));
    ZarrWriteOptions options; options.format = ZarrFormat::v3; write_zarr(path.string(),d,options);

    for (const auto* name : {"X","uns/weights","uns/counts","uns/mask"})
        shard_metadata(path,name,{9,11},{4,6},{2,3});
    shard_metadata(path,"uns/cube",{5,4,7},{4,4,6},{2,2,3});
    shard_metadata(path,"uns/huge",{1000003,1000007},{4,6},{2,3},-7);
    shard_metadata(path,"uns/fill",{5,7},{4,6},{2,3},-19.5);

    anndatapp_zarr::Store store(path,true,128*1024);
    store.write_numeric("X",{0,0},{9,11},f.x.data());
    store.write_numeric("uns/weights",{0,0},{9,11},f.weights.data());
    store.write_numeric("uns/counts",{0,0},{9,11},f.counts.data());
    store.write_numeric("uns/mask",{0,0},{9,11},f.mask.data());
    store.write_numeric("uns/cube",{0,0,0},{5,4,7},f.cube.data());
    const std::int64_t remote = 1234567890123;
    store.write_numeric("uns/huge",{1000002,1000006},{1,1},&remote);
    return f;
}


Fixture ordinary_fixture(const std::filesystem::path& path, ZarrFormat format, ZarrCompression compression,
                         bool big_endian, bool transpose) {
    Fixture f; f.root=path; f.x=sequence<float>(99,0.5f,3.0f);
    auto d=AnnData::empty(9,11); d.set_x(array<float>({9,11},f.x));
    for(std::size_t i=0;i<f.mask.size();++i)f.mask[i]=(i%3)==1;
    auto mask=Array::empty<bool>({9,11});std::copy(f.mask.begin(),f.mask.end(),mask.edit().dense<bool>().data());
    d.set_uns("mask",Element(std::move(mask)));
    ZarrWriteOptions options;options.format=format;options.compression=compression;write_zarr(path.string(),d,options);
    const auto metadata=path/"X"/(format==ZarrFormat::v3?"zarr.json":".zarray");
    auto m=load_json(metadata);
    if(format==ZarrFormat::v3) {
        m["chunk_grid"]["configuration"]["chunk_shape"]={4,6};
        if(big_endian)m["codecs"][0]["configuration"]["endian"]="big";
        if(transpose)m["codecs"].insert(m["codecs"].begin(),Json{{"name","transpose"},{"configuration",{{"order",{1,0}}}}});
    } else {
        m["chunks"]={4,6};
        if(big_endian)m["dtype"]=">f4";
        if(transpose)m["order"]="F";
    }
    save_json(metadata,m);
    const auto consolidated=path/(format==ZarrFormat::v3?"zarr.json":".zmetadata");
    if(std::filesystem::exists(consolidated)) {
        auto root=load_json(consolidated);
        if(format==ZarrFormat::v3 && root.contains("consolidated_metadata"))root["consolidated_metadata"]["metadata"]["X"]=m;
        if(format==ZarrFormat::v2 && root.contains("metadata"))root["metadata"]["X/.zarray"]=m;
        save_json(consolidated,root);
    }
    for(const auto& e:std::filesystem::directory_iterator(path/"X")) {
        const auto name=e.path().filename().string();
        if(name!="zarr.json" && name!=".zarray" && name!=".zattrs")std::filesystem::remove_all(e.path());
    }
    anndatapp_zarr::Store store(path,true,128*1024);store.write_numeric("X",{0,0},{9,11},f.x.data());
    return f;
}

template<class T> void typed_gather(const BackedArray& a, const T* source, std::size_t width) {
    const std::array<std::size_t,3> rows{7,1,7}; const std::array<std::size_t,4> cols{10,0,5,10};
    auto t = a.select({indices{7,1,7},indices{10,0,5,10}}).prepare_transfer(32*1024);
    std::array<T,12> out{}; t.read_into({0,0},{3,4},DenseBuffer::from(out.data(),{3,4}));
    for (std::size_t i = 0; i < rows.size(); ++i) for (std::size_t j = 0; j < cols.size(); ++j)
        check(out[i*cols.size()+j] == source[rows[i]*width+cols[j]],"typed gathered Zarr values differ");
}
void gathered_x(const Fixture& f, const BackedAnnData& d) {
    const std::array<std::size_t,4> rows{8,0,5,8}; const std::array<std::size_t,5> cols{10,1,6,0,10};
    auto a = d.x().select({indices{8,0,5,8},indices{10,1,6,0,10}});
    auto t = a.prepare_transfer(32*1024);
    check(t.scratch_capacity() == 32*1024,"small transfer budget differs");
    // Warm the backend with an overlapping destination, which uses the
    // logical-order fallback. The next grouped read needs a larger tile.
    float warm = 0;
    t.read_into({0,0},{4,5},DenseBuffer::from(&warm,{4,5},{0,0}));
    const auto allocations = t.stats().scratch_allocations;
    const auto reads = t.stats().read_calls;
    std::array<float,20> grouped{};
    t.read_into({0,0},{4,5},DenseBuffer::from(grouped.data(),{4,5}));
    check(t.stats().read_calls-reads < 20,"read-only gather did not group by inner chunk");
    check(t.stats().scratch_allocations == allocations+1,"grouped tile replacement allocation not counted");
    t.read_into({0,0},{4,5},DenseBuffer::from(grouped.data(),{4,5}));
    check(t.stats().scratch_allocations == allocations+1,"reused grouped tile counted as a new allocation");
    std::array<float,6> block{}; t.read_into({1,1},{2,3},DenseBuffer::from(block.data(),{2,3}));
    for (std::size_t i = 0; i < 2; ++i) for (std::size_t j = 0; j < 3; ++j)
        check(block[i*3+j] == f.x[rows[i+1]*11+cols[j+1]],"partial logical gather differs");
    auto low = a.prepare_transfer(2048); std::array<float,20> fallback{};
    low.read_into({0,0},{4,5},DenseBuffer::from(fallback.data(),{4,5}));
    for (std::size_t i = 0; i < 4; ++i) for (std::size_t j = 0; j < 5; ++j)
        check(fallback[i*5+j] == f.x[rows[i]*11+cols[j]],"low-budget gather differs");

    std::vector<float> column(20,-999.0f);
    a.read_into(DenseBuffer::from(column.data(),{4,5},{std::ptrdiff_t(sizeof(float)),std::ptrdiff_t(4*sizeof(float))}));
    for (std::size_t i = 0; i < 4; ++i) for (std::size_t j = 0; j < 5; ++j)
        check(column[i+j*4] == f.x[rows[i]*11+cols[j]],"column-major output differs");

    std::vector<float> negative(20,-999.0f);
    a.read_into(DenseBuffer::from(negative.data()+19,{4,5},{-std::ptrdiff_t(5*sizeof(float)),-std::ptrdiff_t(sizeof(float))}));
    for (std::size_t i = 0; i < 4; ++i) for (std::size_t j = 0; j < 5; ++j)
        check(negative[(3-i)*5+4-j] == f.x[rows[i]*11+cols[j]],"negative output strides differ");

    std::vector<float> holes(69,-999.0f);
    a.read_into(DenseBuffer::from(holes.data(),{4,5},{std::ptrdiff_t(20*sizeof(float)),std::ptrdiff_t(2*sizeof(float))}));
    for (std::size_t i = 0; i < holes.size(); ++i) {
        bool written = false;
        for (std::size_t r = 0; r < 4; ++r) for (std::size_t c = 0; c < 5; ++c)
            written = written || i == r*20+c*2;
        check(holes[i] == (written ? f.x[rows[(i/20)]*11+cols[(i%20)/2]] : -999.0f),"strided output hole changed");
    }

    float alias = -1;
    a.read_into(DenseBuffer::from(&alias,{4,5},{0,0}));
    check(alias == f.x[rows.back()*11+cols.back()],"zero-stride destination lost last C-order write");
    std::array<float,8> overlap{}; overlap.fill(-999.0f);
    auto expected = overlap;
    for (std::size_t i = 0; i < 4; ++i) for (std::size_t j = 0; j < 5; ++j)
        expected[i+j] = f.x[rows[i]*11+cols[j]];
    a.read_into(DenseBuffer::from(overlap.data(),{4,5},{std::ptrdiff_t(sizeof(float)),std::ptrdiff_t(sizeof(float))}));
    check(overlap == expected,"overlapping output strides changed logical write order");

    auto materialized = a.read(); auto mv = materialized.view().dense<float>();
    check(mv.extent(0) == 4 && mv.extent(1) == 5,"materialized gather shape differs");
    for (std::size_t i = 0; i < 4; ++i) for (std::size_t j = 0; j < 5; ++j)
        check(mv.data()[i*5+j] == f.x[rows[i]*11+cols[j]],"gather materialization differs");

    auto reverse = d.x().select({range{8,1,-2},range{10,0,-3}});
    std::array<float,16> reversed{}; reverse.prepare_transfer(32*1024).read_into({0,0},{4,4},DenseBuffer::from(reversed.data(),{4,4}));
    for (std::size_t i = 0; i < 4; ++i) for (std::size_t j = 0; j < 4; ++j)
        check(reversed[i*4+j] == f.x[(8-2*i)*11+(10-3*j)],"reverse affine gather differs");
}

void ordinary_chunks(const std::filesystem::path& root, ZarrFormat format, ZarrCompression compression,
                     bool big_endian=false, bool transpose=false) {
    auto f=ordinary_fixture(root,format,compression,big_endian,transpose);
    auto d=open_zarr(root.string());
    gathered_x(f,d);
    typed_gather(d.uns().at("mask").array(),f.mask.data(),11);
    auto a=d.x().select({indices{8,0,5,8},indices{10,1,6,0,10}});
    auto t=a.prepare_transfer(32*1024);std::array<float,20> out{};
    const auto before=t.stats().read_calls;
    t.read_into({0,0},{4,5},DenseBuffer::from(out.data(),{4,5}));
    check(t.stats().read_calls-before==6,"ordinary grouped read must request each touched chunk once");
    std::array<float,20> prepared_values{};auto prepared=a.prepare_read();
    auto prepared_buffer=DenseBuffer::from(prepared_values.data(),{4,5});
    prepared.preflight(prepared_buffer);prepared.read_into(prepared_buffer);
    check(prepared_values==out,"ordinary prepared read differs");
    for(auto mode:{StreamMode::manual,StreamMode::adaptive}) {
        StreamOptions options;options.mode=mode;options.min_block_bytes=16;options.target_block_bytes=32;
        options.max_block_bytes=64;options.scratch_bytes_per_worker=32*1024;
        BlockStream stream(t.geometry(),options);std::array<float,20> streamed{};std::size_t cells=0;
        while(auto request=stream.next()) {
            const auto n=request->count[0]*request->count[1];std::vector<float> block(n);
            t.read_into(request->start,request->count,DenseBuffer::from(block.data(),request->count));
            for(std::size_t i=0;i<request->count[0];++i)for(std::size_t j=0;j<request->count[1];++j)
                streamed[(request->start[0]+i)*5+request->start[1]+j]=block[i*request->count[1]+j];
            cells+=n;stream.complete(*request,n*sizeof(float),0.001);
        }
        check(stream.finished() && cells==20 && streamed==out,"ordinary streamed selection differs");
    }
    auto empty=d.x().select({indices{},all}).prepare_transfer(32*1024);
    empty.read_into({0,0},{0,11},DenseBuffer::from(static_cast<float*>(nullptr),{0,11}));
    check(empty.stats().read_calls==0,"empty ordinary selection performs I/O");
    // Export with a different validation mode uses a BorrowedBackend wrapper.
    ZarrWriteOptions exported;exported.format=format;exported.validation=Validation::none;
    const auto destination=root.parent_path()/(root.filename().string()+"-export");
    write_zarr(destination.string(),d.slice(indices{8,0,5,8},indices{10,1,6,0,10}),exported);
    auto copied=open_zarr(destination.string());std::array<float,20> exported_values{};
    copied.x().read_into(DenseBuffer::from(exported_values.data(),{4,5}));
    check(exported_values==out,"borrowed validation export differs");
    // Remove a boundary chunk and ensure missing-chunk fill survives grouping.
    const auto missing=format==ZarrFormat::v3?root/"X/c/2/1":root/"X/2.1";
    check(std::filesystem::remove(missing),"expected boundary chunk is absent");
    std::array<float,20> fill{};a.read_into(DenseBuffer::from(fill.data(),{4,5}));
    const std::array<std::size_t,4> rows{8,0,5,8};const std::array<std::size_t,5> cols{10,1,6,0,10};
    for(std::size_t i=0;i<4;++i)for(std::size_t j=0;j<5;++j)
        check(fill[i*5+j]==(rows[i]>=8 && cols[j]>=6?0.0f:f.x[rows[i]*11+cols[j]]),"ordinary missing chunk fill differs");
    if(format==ZarrFormat::v3) {
        // The existing one-shot decoder does not parse codecs for absent chunks.
        const auto file=root/"X/zarr.json";auto m=load_json(file);
        m["codecs"].push_back(Json{{"name","unsupported-test-codec"},{"configuration",Json::object()}});save_json(file,m);
        auto root_metadata=load_json(root/"zarr.json");
        if(root_metadata.contains("consolidated_metadata"))root_metadata["consolidated_metadata"]["metadata"]["X"]=m;
        save_json(root/"zarr.json",root_metadata);
        auto missing_store=open_zarr(root.string());
        auto missing_array=missing_store.x().select({indices{8,8},indices{10,9}});
        std::array<float,4> values{1,1,1,1};auto buffer=DenseBuffer::from(values.data(),{2,2});
        missing_array.read_into(buffer);
        check(values==std::array<float,4>{0,0,0,0},"missing chunk one-shot parsed unsupported codec");
        values.fill(1);auto read=missing_array.prepare_read();read.preflight(buffer);read.read_into(buffer);
        check(values==std::array<float,4>{0,0,0,0},"missing chunk prepared read parsed unsupported codec");
        bool rejected=false;try{auto context=missing_array.prepare_transfer(32*1024);(void)context;}
        catch(const Error&){rejected=true;}
        check(rejected,"transfer context did not preserve eager codec validation");
    }
}


void tight_scratch_fallback(const std::filesystem::path& root) {
    auto d=AnnData::empty(2,4096);auto values=Array::empty<float>({2,4096});
    std::fill_n(values.edit().dense<float>().data(),8192,0.0f);d.set_x(std::move(values));
    ZarrWriteOptions options;options.compression=ZarrCompression::none;write_zarr(root.string(),d,options);
    const auto file=root/"X/zarr.json";auto m=load_json(file);
    m["chunk_grid"]["configuration"]["chunk_shape"]={1,4};save_json(file,m);
    auto meta=load_json(root/"zarr.json");
    if(meta.contains("consolidated_metadata"))meta["consolidated_metadata"]["metadata"]["X"]=m;
    save_json(root/"zarr.json",meta);std::filesystem::remove_all(root/"X/c");
    anndatapp_zarr::Store store(root,true,32*1024);const std::array<float,4> first{7,8,9,10};
    store.write_numeric("X",{0,0},{1,4},first.data());
    auto backed=open_zarr(root.string());auto a=backed.x().select({all,range{0,4096,2}});
    auto t=a.prepare_transfer(32*1024);
    check(t.stats().scratch_bytes==8192,"tight context did not retain its full packing tile");
    std::vector<float> out(8192,-1.0f);
    t.read_into({0,0},{2,2048},DenseBuffer::from(out.data(),{2,2048},{16384,8}));
    check(t.stats().read_calls==4096,"tight context should use ordered element fallback");
    for(std::size_t i=0;i<2;++i)for(std::size_t j=0;j<2048;++j) {
        const auto expected=i==0 && j==0?7.0f:i==0 && j==1?9.0f:0.0f;
        check(out[i*4096+j*2]==expected && out[i*4096+j*2+1]==-1.0f,"tight nonpacked fallback differs");
    }
}
void writable_roundtrip(const std::filesystem::path& root) {
    BackedOptions options;options.mode=AccessMode::read_write;
    auto d=open_zarr(root.string(),options);
    auto a=d.x().select({indices{8,0,5},indices{10,1,6,0}});auto t=a.prepare_transfer(64*1024);
    std::array<float,12> values{};for(std::size_t i=0;i<values.size();++i)values[i]=float(1000+i);
    t.write_from({0,0},{3,4},DenseBuffer::from(values.data(),{3,4}));
    std::array<float,12> out{};t.read_into({0,0},{3,4},DenseBuffer::from(out.data(),{3,4}));
    check(out==values,"writable context read after write differs");
    check(t.stats().scratch_bytes<=64*1024,"writable context retained scratch exceeds budget");
}

void higher_ranks_and_dtypes(const Fixture& f, const BackedAnnData& d) {
    typed_gather(d.uns().at("weights").array(),f.weights.data(),11);
    typed_gather(d.uns().at("counts").array(),f.counts.data(),11);
    typed_gather(d.uns().at("mask").array(),f.mask.data(),11);
    auto cube = d.uns().at("cube").array().select({indices{4,0,4},indices{3,1},indices{6,0,6}});
    std::array<float,18> out{}; cube.prepare_transfer(32*1024).read_into({0,0,0},{3,2,3},DenseBuffer::from(out.data(),{3,2,3}));
    const std::array<std::size_t,3> r{4,0,4}; const std::array<std::size_t,2> z{3,1}; const std::array<std::size_t,3> c{6,0,6};
    for (std::size_t i=0;i<3;++i) for (std::size_t j=0;j<2;++j) for (std::size_t k=0;k<3;++k)
        check(out[(i*2+j)*3+k] == f.cube[(r[i]*4+z[j])*7+c[k]],"rank-three gathered values differ");
}
void sparse_huge_and_fill(const BackedAnnData& d) {
    auto huge = d.uns().at("huge").array().select({indices{1000002,0,1000000},indices{1000006,1,1000003}});
    std::array<std::int64_t,9> out{}; huge.prepare_transfer(32*1024).read_into({0,0},{3,3},DenseBuffer::from(out.data(),{3,3}));
    const std::array<std::int64_t,9> expected{1234567890123,-7,-7,-7,-7,-7,-7,-7,-7};
    check(out == expected,"huge sparse gaps or missing inner chunks differ from fill");
    auto fill = d.uns().at("fill").array().select({indices{4,0,4},indices{6,1}});
    std::array<double,6> empty{}; fill.prepare_transfer(32*1024).read_into({0,0},{3,2},DenseBuffer::from(empty.data(),{3,2}));
    check(std::all_of(empty.begin(),empty.end(),[](double x){return x == -19.5;}),"absent shards do not return fill value");
    std::int32_t scalar = 0; auto s = d.uns().at("scalar").array().prepare_transfer(32*1024);
    s.read_into({}, {}, DenseBuffer::from(&scalar,{})); check(scalar == 37,"scalar read differs");
    auto e = d.uns().at("empty").array().prepare_transfer(32*1024);
    e.read_into({0,0},{0,3},DenseBuffer::from(static_cast<double*>(nullptr),{0,3}));
    check(e.stats().read_calls == 0,"empty transfer performs I/O");
}
}
int main(int argc, char** argv) {
    try {
        auto root = std::filesystem::path(argc > 1 ? argv[1] : "gather-transfer-tests");
        root /= "run-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        auto f = fixture(root); auto d = open_zarr(root.string());
        gathered_x(f,d); higher_ranks_and_dtypes(f,d); sparse_huge_and_fill(d);
        d.close();writable_roundtrip(root);
        tight_scratch_fallback(root.parent_path()/(root.filename().string()+"-tight-scratch"));
        ordinary_chunks(root.parent_path()/(root.filename().string()+"-ordinary-v2-raw"),ZarrFormat::v2,ZarrCompression::none);
        ordinary_chunks(root.parent_path()/(root.filename().string()+"-ordinary-v2-zstd-fortran"),ZarrFormat::v2,ZarrCompression::zstd,true,true);
        ordinary_chunks(root.parent_path()/(root.filename().string()+"-ordinary-v3-raw-transpose"),ZarrFormat::v3,ZarrCompression::none,false,true);
        ordinary_chunks(root.parent_path()/(root.filename().string()+"-ordinary-v3-zstd-big"),ZarrFormat::v3,ZarrCompression::zstd,true,false);
        std::cout << "gathered sharded and ordinary chunk transfers passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
