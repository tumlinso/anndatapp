#include <hdf5.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

import anndatapp;
import anndatapp.dataset;
using namespace anndatapp;

void require(bool ok,const char* text) { if (!ok) throw std::runtime_error(text); }
template<class F> void rejects(F&& f) {
    try { f(); } catch (const Error&) { return; }
    throw std::runtime_error("expected index rejection");
}
template<class F> void expect_code(ErrorCode code,F&& f) {
    try { f(); }
    catch (const Error& e) {
        require(e.code()==code,"unexpected error code");
        return;
    }
    throw std::runtime_error("expected anndatapp error");
}
void replace_index(hid_t file,const std::vector<std::string>& names,bool fixed,
                   std::size_t width=48,const char* table="/obs") {
    const auto index=std::string(table)+"/_index";
    require(H5Ldelete(file,index.c_str(),H5P_DEFAULT)>=0,"replace raw string fixture");
    auto type=H5Tcopy(H5T_C_S1);
    require(H5Tset_size(type,fixed?width:H5T_VARIABLE)>=0,"set string width");
    require(H5Tset_cset(type,H5T_CSET_UTF8)>=0,"set string encoding");
    hsize_t n=names.size();auto space=H5Screate_simple(1,&n,nullptr);
    auto data=H5Dcreate2(file,index.c_str(),type,space,H5P_DEFAULT,H5P_DEFAULT,H5P_DEFAULT);
    require(data>=0,"create raw string fixture");
    const char* keys[]{"encoding-type","encoding-version"};
    const std::string values[]{"string-array","0.2.0"};
    auto scalar=H5Screate(H5S_SCALAR);
    for (int i=0;i<2;++i) {
        auto t=H5Tcopy(H5T_C_S1);H5Tset_size(t,values[i].size());
        auto attr=H5Acreate2(data,keys[i],t,scalar,H5P_DEFAULT,H5P_DEFAULT);
        require(attr>=0 && H5Awrite(attr,t,values[i].data())>=0,"write encoding attribute");
        H5Aclose(attr);H5Tclose(t);
    }
    H5Sclose(scalar);
    if (fixed) {
        std::vector<char> bytes(names.size()*width,0);
        for (std::size_t i=0;i<names.size();++i) names[i].copy(bytes.data()+i*width,width);
        require(H5Dwrite(data,type,H5S_ALL,H5S_ALL,H5P_DEFAULT,bytes.data())>=0,"write fixed strings");
    } else {
        std::vector<const char*> values;
        for (const auto& name:names) values.push_back(name.c_str());
        require(H5Dwrite(data,type,H5S_ALL,H5S_ALL,H5P_DEFAULT,values.data())>=0,"write variable strings");
    }
    H5Dclose(data);H5Sclose(space);H5Tclose(type);
}
void replace_index(const std::string& path,const std::vector<std::string>& names,bool fixed,std::size_t width=48) {
    auto file=H5Fopen(path.c_str(),H5F_ACC_RDWR,H5P_DEFAULT);
    require(file>=0,"open raw string fixture");
    replace_index(file,names,fixed,width);
    require(H5Fclose(file)>=0,"close raw string fixture");
}
void corrupt_var_index_version(const std::string& path) {
    auto file=H5Fopen(path.c_str(),H5F_ACC_RDWR,H5P_DEFAULT);
    require(file>=0,"open component index for corruption");
    auto data=H5Dopen2(file,"/var/_index",H5P_DEFAULT);
    require(data>=0 && H5Adelete(data,"encoding-version")>=0,"remove index version");
    auto type=H5Tcopy(H5T_C_S1);auto space=H5Screate(H5S_SCALAR);
    require(type>=0 && space>=0 && H5Tset_size(type,5)>=0,"create corrupt index version type");
    auto attr=H5Acreate2(data,"encoding-version",type,space,H5P_DEFAULT,H5P_DEFAULT);
    require(attr>=0 && H5Awrite(attr,type,"9.0.0")>=0,"write corrupt index version");
    H5Aclose(attr);H5Sclose(space);H5Tclose(type);H5Dclose(data);
    require(H5Fflush(file,H5F_SCOPE_GLOBAL)>=0,"flush corrupt index version");
    H5Fclose(file);
}
void set_string_attr(hid_t object,const char* name,const char* value) {
    const auto exists=H5Aexists(object,name);
    require(exists>=0,"inspect X encoding attribute");
    if(exists) require(H5Adelete(object,name)>=0,"delete X encoding attribute");
    auto type=H5Tcopy(H5T_C_S1);
    require(type>=0 && H5Tset_size(type,std::strlen(value))>=0,"set X encoding attribute type");
    auto space=H5Screate(H5S_SCALAR);
    auto attr=H5Acreate2(object,name,type,space,H5P_DEFAULT,H5P_DEFAULT);
    require(space>=0 && attr>=0 && H5Awrite(attr,type,value)>=0,"write X encoding attribute");
    H5Aclose(attr);H5Sclose(space);H5Tclose(type);
}
void write_metadata_component(const std::filesystem::path& path,Layout layout,const char* prefix) {
    auto data=AnnData::empty(2,3);
    data.set_obs(Table::with_index({std::string(prefix)+"-0",std::string(prefix)+"-1"}));
    data.set_var(Table::with_index({"v0","v1","v2"}));
    if(layout==Layout::dense) data.set_x(Array::zeros<float>({2,3}));
    else if(layout==Layout::csr) {
        auto x=Array::csr<float,std::int32_t,std::int32_t>(2,3,2);
        auto v=x.edit().csr<float,std::int32_t,std::int32_t>();
        v.values()[0]=1;v.values()[1]=2;v.unsafe_indices()[0]=0;v.unsafe_indices()[1]=1;
        v.unsafe_offsets()[0]=0;v.unsafe_offsets()[1]=1;v.unsafe_offsets()[2]=2;
        data.set_x(std::move(x));
    } else {
        auto x=Array::csc<float,std::int32_t,std::int32_t>(2,3,2);
        auto v=x.edit().csc<float,std::int32_t,std::int32_t>();
        v.values()[0]=1;v.values()[1]=2;v.unsafe_indices()[0]=0;v.unsafe_indices()[1]=1;
        v.unsafe_offsets()[0]=0;v.unsafe_offsets()[1]=1;v.unsafe_offsets()[2]=1;v.unsafe_offsets()[3]=2;
        data.set_x(std::move(x));
    }
    write_h5ad(path.string(),data);
}
void write_metadata_components(const std::filesystem::path& dir,Layout layout) {
    std::filesystem::create_directories(dir);
    write_metadata_component(dir/"first.h5ad",layout,"first");
    write_metadata_component(dir/"second.h5ad",layout,"second");
}
void replace_dense_x_shape(hid_t file) {
    require(H5Ldelete(file,"/X",H5P_DEFAULT)>=0,"replace X shape fixture");
    hsize_t dims[]{1,3};
    auto space=H5Screate_simple(2,dims,nullptr);
    auto x=H5Dcreate2(file,"/X",H5T_IEEE_F32LE,space,H5P_DEFAULT,H5P_DEFAULT,H5P_DEFAULT);
    const float values[]{7,8,9};
    require(space>=0 && x>=0 && H5Dwrite(x,H5T_NATIVE_FLOAT,H5S_ALL,H5S_ALL,H5P_DEFAULT,values)>=0,
            "write wrong-shape X fixture");
    set_string_attr(x,"encoding-type","array");set_string_attr(x,"encoding-version","0.2.0");
    H5Dclose(x);H5Sclose(space);
}
void replace_sparse_indices_length(hid_t file) {
    require(H5Ldelete(file,"/X/indices",H5P_DEFAULT)>=0,"replace sparse indices fixture");
    hsize_t n=1;
    auto space=H5Screate_simple(1,&n,nullptr);
    auto x=H5Dcreate2(file,"/X/indices",H5T_STD_I32LE,space,H5P_DEFAULT,H5P_DEFAULT,H5P_DEFAULT);
    const std::int32_t index=0;
    require(space>=0 && x>=0 && H5Dwrite(x,H5T_NATIVE_INT32,H5S_ALL,H5S_ALL,H5P_DEFAULT,&index)>=0,
            "write shortened sparse indices fixture");
    H5Dclose(x);H5Sclose(space);
}
void malformed_unselected_x_metadata(const std::filesystem::path& root) {
    const auto stamp=std::chrono::steady_clock::now().time_since_epoch().count();
    const char* labels[]{"shape","encoding","version","sparse-buffers"};
    const ErrorCode codes[]{ErrorCode::shape_mismatch,ErrorCode::unsupported_encoding,
                            ErrorCode::unsupported_version,ErrorCode::invalid_sparse_structure};
    for(int kind=0;kind<4;++kind) {
        auto dir=root/(std::string("unselected-x-")+labels[kind]+"-"+std::to_string(stamp));
        auto layout=kind==3?Layout::csr:Layout::dense;
        write_metadata_components(dir,layout);
        auto first=(dir/"first.h5ad").string();
        auto writable=H5Fopen(first.c_str(),H5F_ACC_RDWR,H5P_DEFAULT);
        require(writable>=0,"open X metadata writer before collection");
        auto data=create_h5ads((dir/"data.h5ads").string(),
            {{"first","first.h5ad"},{"second","second.h5ad"}},"sample");
        if(kind==0) replace_dense_x_shape(writable);
        else if(kind==1 || kind==2) {
            auto x=H5Dopen2(writable,"/X",H5P_DEFAULT);
            require(x>=0,"open X encoding fixture");
            set_string_attr(x,kind==1?"encoding-type":"encoding-version",kind==1?"unknown-array":"9.0.0");
            H5Dclose(x);
        } else replace_sparse_indices_length(writable);
        require(H5Fflush(writable,H5F_SCOPE_GLOBAL)>=0,"flush malformed X metadata");

        auto selected=data.slice(indices{2},all);
        float dense[3]{-7,-7,-7};
        if(kind<3) {
            expect_code(codes[kind],[&] { selected.read_x_into(DenseBuffer::from(dense,{1,3})); });
            require(dense[0]==-7 && dense[1]==-7 && dense[2]==-7,"malformed X metadata changed dense output");
        } else {
            float values[8];std::int32_t positions[8],offsets[8];
            std::fill_n(values,8,-7);std::fill_n(positions,8,-7);std::fill_n(offsets,8,-7);
            SparseBuffer out{values,positions,offsets,DType::float32,DType::int32,DType::int32,8,8,8};
            expect_code(codes[kind],[&] { selected.read_x_into(out); });
            require(std::all_of(values,values+8,[](float x){return x==-7;}) &&
                    std::all_of(positions,positions+8,[](std::int32_t x){return x==-7;}) &&
                    std::all_of(offsets,offsets+8,[](std::int32_t x){return x==-7;}),
                    "malformed sparse metadata changed output");
        }
        data.close();
        require(H5Fclose(writable)>=0,"close malformed X metadata writer");
    }
}
void unselected_reference_payload_not_scanned(const std::filesystem::path& root) {
    const auto stamp=std::chrono::steady_clock::now().time_since_epoch().count();
    auto dir=root/("unselected-x-payload-"+std::to_string(stamp));
    write_metadata_components(dir,Layout::csr);
    auto first=(dir/"first.h5ad").string();
    auto writable=H5Fopen(first.c_str(),H5F_ACC_RDWR,H5P_DEFAULT);
    require(writable>=0,"open sparse payload writer before collection");
    auto data=create_h5ads((dir/"data.h5ads").string(),
        {{"first","first.h5ad"},{"second","second.h5ad"}},"sample");
    auto indices_data=H5Dopen2(writable,"/X/indices",H5P_DEFAULT);
    const std::int32_t bad[]{99,1};
    require(indices_data>=0 && H5Dwrite(indices_data,H5T_NATIVE_INT32,H5S_ALL,H5S_ALL,H5P_DEFAULT,bad)>=0,
            "corrupt sparse payload fixture");
    H5Dclose(indices_data);
    require(H5Fflush(writable,H5F_SCOPE_GLOBAL)>=0,"flush corrupted sparse payload");

    auto later=data.slice(indices{2},all).prepare_x();
    auto r=later.requirements();
    require(r.layout==Layout::csr && r.shape==std::vector<std::size_t>({1,3}) && r.nnz==1,
            "later-only sparse selection scanned unselected reference payload");
    float values[2]{-7,-7};std::int32_t positions[2]{-7,-7},offsets[2]{-7,-7};
    SparseBuffer out{values,positions,offsets,r.dtype,r.index_dtype,r.offset_dtype,2,2,2};
    later.read_into(out);
    require(values[0]==1 && positions[0]==0 && offsets[0]==0 && offsets[1]==1,
            "later-only sparse transfer differs with corrupt unselected payload");

    std::fill_n(values,2,-7);std::fill_n(positions,2,-7);std::fill_n(offsets,2,-7);
    expect_code(ErrorCode::invalid_sparse_structure,[&] { data.slice(indices{0},all).read_x_into(out); });
    require(std::all_of(values,values+2,[](float x){return x==-7;}) &&
            std::all_of(positions,positions+2,[](std::int32_t x){return x==-7;}) &&
            std::all_of(offsets,offsets+2,[](std::int32_t x){return x==-7;}),
            "selected malformed sparse payload changed output");
    data.close();
    require(H5Fclose(writable)>=0,"close sparse payload writer");
}
void empty_dataset_invalid_peer(const std::filesystem::path& root) {
    const auto stamp=std::chrono::steady_clock::now().time_since_epoch().count();
    auto dir=root/("empty-peer-"+std::to_string(stamp));
    std::filesystem::create_directories(dir);
    for (int i=0;i<2;++i) {
        auto data=AnnData::empty(1,3);
        data.set_obs(Table::with_index({i ? "cell-1" : "cell-0"}));
        data.set_var(Table::with_index({"v0","v1","v2"}));
        data.set_x(Array::zeros<float>({1,3}));
        write_h5ad((dir/(i ? "dataset-right.h5ad" : "dataset-left.h5ad")).string(),data);
    }
    // Components are opened read-only by H5ADS; retain a writable HDF5 handle
    // before opening the collection to emulate a separately changed component.
    auto writable=H5Fopen((dir/"dataset-right.h5ad").c_str(),H5F_ACC_RDWR,H5P_DEFAULT);
    require(writable>=0,"open writable component before collection");
    DatasetOptions options;options.backed.mode=AccessMode::read_write;options.check_axis_names=true;
    auto data=create_h5ads((dir/"empty-peer.h5ads").string(),
        {{"left","dataset-left.h5ad"},{"right","dataset-right.h5ad"}},"sample",options);
    auto selected=data.slice(indices{0,1},range{0,0});
    corrupt_var_index_version((dir/"dataset-right.h5ad").string());
    float sentinel=-1;
    try {
        selected.read_x_into(DenseBuffer::from(&sentinel,{2,0}));
        throw std::runtime_error("empty selected variables accepted malformed component index");
    } catch (const Error& e) {
        require(e.code()==ErrorCode::unsupported_version,"collection lost malformed peer version error");
    }
    require(sentinel==-1,"empty selected variables changed output sentinel");
    data.close();H5Fclose(writable);
}
void shorter_var_index_after_snapshot(const std::filesystem::path& root) {
    const auto dir=root/"short-var-index";
    std::filesystem::create_directories(dir);
    const auto manifest_path=(dir/"manifest.h5ad").string();
    const auto component_path=(dir/"component.h5ad").string();
    const std::vector<std::string> names{"gene-0","gene-1","gene-2"};
    auto data=AnnData::empty(0,names.size());
    data.set_var(Table::with_index(names));
    write_h5ad(manifest_path,data);write_h5ad(component_path,data);

    // Keep a writer open before the read-only backed snapshots are created.
    auto writable=H5Fopen(component_path.c_str(),H5F_ACC_RDWR,H5P_DEFAULT);
    require(writable>=0,"open writable component before backed snapshot");
    auto manifest=open_h5ad(manifest_path);
    BackedAnnData files[]{open_h5ad(component_path)};

    // The replacement remains a valid string-array 0.2 index, but its stored
    // length no longer matches the already-open component's var axis.
    replace_index(writable,{"gene-0","gene-1"},false,48,"/var");
    require(H5Fflush(writable,H5F_SCOPE_GLOBAL)>=0,"flush shortened component index");
    const std::size_t ids[]{0};
    try {
        detail::compare_backed_component_indexes(manifest,files,1,ids,1,range{0,0});
        throw std::runtime_error("empty variable selection accepted a shortened component index");
    } catch (const Error& e) {
        require(e.code()==ErrorCode::shape_mismatch,"shortened component index lost its shape error");
    }
    files[0].close();manifest.close();
    require(H5Fclose(writable)>=0,"close writable component");
}
int main(int argc,char** argv) {
    try {
        if (argc!=2) throw std::runtime_error("output directory required");
        std::filesystem::create_directories(argv[1]);
        malformed_unselected_x_metadata(argv[1]);
        unselected_reference_payload_not_scanned(argv[1]);
        empty_dataset_invalid_peer(argv[1]);
        shorter_var_index_after_snapshot(argv[1]);
        const auto path=(std::filesystem::path(argv[1])/"index.h5ad").string();
        auto a=AnnData::empty(128,0);
        std::vector<std::string> names;
        for (int i=0;i<128;++i) names.push_back("cell-"+std::to_string(i)+std::string(31,'x'));
        a.set_obs(Table::with_index(names));
        write_h5ad(path,a);
        BackedOptions options;options.scratch_bytes=4096;
        auto f=open_h5ad(path,options),g=open_h5ad(path,options);
        auto l=f.obs().select(indices{127,0,93,0,1,93,126,64,63,64});
        auto r=g.obs().select(indices{127,0,93,0,1,93,126,64,63,64});
        const auto expected_path=(std::filesystem::path(argv[1])/"expected.h5ad").string();
        auto expected=AnnData::empty(10,0);
        std::vector<std::string> selected;
        for (auto i:{127,0,93,0,1,93,126,64,63,64}) selected.push_back(names[i]);
        expected.set_obs(Table::with_index(selected));write_h5ad(expected_path,expected);
        auto oracle=open_h5ad(expected_path,options);
        detail::compare_backed_table_indexes(l,oracle.obs());
        detail::compare_backed_table_indexes(l,r);
        BackedTable targets[]{r,r};
        detail::compare_backed_table_indexes(l,targets,2);
        // The collection bridge owns IDs, not an array of selected table nodes.
        const auto component_left=(std::filesystem::path(argv[1])/"component-left.h5ad").string();
        const auto component_right=(std::filesystem::path(argv[1])/"component-right.h5ad").string();
        auto component=AnnData::empty(0,names.size());component.set_var(Table::with_index(names));
        write_h5ad(component_left,component);write_h5ad(component_right,component);
        auto component_options=options;component_options.mode=AccessMode::read_write;
        BackedAnnData files[]{open_h5ad(component_left,component_options),open_h5ad(component_right,component_options)};
        const std::size_t ids[]{1,0,1};
        const Selection columns=indices{127,0,93,0,1,93,126,64,63,64};
        detail::compare_backed_component_indexes(oracle.obs(),files,2,ids,3,columns);
        detail::compare_backed_component_indexes(oracle.obs(),nullptr,0,nullptr,0,columns);
        detail::compare_backed_component_indexes(oracle.obs().select(range{0,0}),files,2,ids,3,range{0,0});
        rejects([&] { detail::compare_backed_component_indexes(oracle.obs().select(range{0,0}),files,2,ids,3,indices{0}); });
        detail::compare_backed_component_indexes(files[0],files,2,ids,3,columns);
        detail::compare_backed_component_indexes(files[0],files,2,ids,3,range{0,0});
        const std::size_t obs_offsets[]{0,128};
        const std::size_t obs_positions[]{127,0,93,0,1,93,126,64,63,64};
        BackedAnnData obs_peer[]{open_h5ad(path,options)};
        detail::compare_backed_observation_indexes(f,obs_peer,1,obs_offsets,Axis::gather(obs_positions,10),0);
        obs_peer[0].close();
        const std::size_t bad_id[]{2};
        rejects([&] { detail::compare_backed_component_indexes(oracle.obs(),files,2,bad_id,1,columns); });
        rejects([&] { detail::compare_backed_component_indexes(oracle.obs(),files,2,ids,3,indices{127,0,93,0,2,93,126,64,63,64}); });
        corrupt_var_index_version(component_right);
        try {
            detail::compare_backed_component_indexes(oracle.obs().select(range{0,0}),files,2,ids,3,range{0,0});
            throw std::runtime_error("empty reference accepted a malformed peer index");
        } catch (const Error& e) {
            require(e.code()==ErrorCode::unsupported_version,"empty reference lost peer index version error");
        }
        try {
            detail::compare_backed_component_indexes(files[0],files,2,ids,3,range{0,0});
            throw std::runtime_error("empty variable bridge accepted malformed peer index");
        } catch (const Error& e) {
            require(e.code()==ErrorCode::unsupported_version,"empty variable bridge lost peer version error");
        }
        files[1].close();
        rejects([&] { detail::compare_backed_component_indexes(oracle.obs(),files,2,ids,3,columns); });
        auto wrong=g.obs().select(indices{127,0,93,0,2,93,126,64,63,64});
        rejects([&] { detail::compare_backed_table_indexes(l,wrong); });
        auto reverse=f.obs().select(range{{},{},-3});
        auto same=g.obs().select(range{{},{},-3});
        detail::compare_backed_table_indexes(reverse,same);
        auto empty=f.obs().select(range{0,0});
        detail::compare_backed_table_indexes(empty,empty);
        detail::compare_backed_table_indexes(l,nullptr,0);
        options.scratch_bytes=64;
        auto small=open_h5ad(path,options);
        rejects([&] { detail::compare_backed_table_indexes(small.obs(),small.obs()); });
        f.close();g.close();small.close();
        replace_index(path,names,true);
        auto fixed=open_h5ad(path,BackedOptions{});
        detail::compare_backed_table_indexes(fixed.obs().select(indices{127,0,93,0,1,93,126,64,63,64}),oracle.obs());
        fixed.close();
        names[93]=std::string("\xff",1);replace_index(path,names,false);
        auto invalid=open_h5ad(path,BackedOptions{});
        rejects([&] { detail::compare_backed_table_indexes(invalid.obs().select(indices{93}),invalid.obs().select(indices{93})); });
        invalid.close();names[93]=std::string(8192,'x');replace_index(path,names,false);
        options.scratch_bytes=4096;auto large=open_h5ad(path,options);
        rejects([&] { detail::compare_backed_table_indexes(large.obs().select(indices{93}),large.obs().select(indices{93})); });
        // Preserve the old fixed-string path when coordinate staging cannot fit:
        // each cursor has 90 bytes, enough for one width-one string but no gather.
        auto tiny=AnnData::empty(1,0);tiny.set_obs(Table::with_index({"a"}));
        const auto tiny_left=(std::filesystem::path(argv[1])/"tiny-left.h5ad").string();
        const auto tiny_right=(std::filesystem::path(argv[1])/"tiny-right.h5ad").string();
        write_h5ad(tiny_left,tiny);write_h5ad(tiny_right,tiny);
        replace_index(tiny_left,{"a"},true,1);replace_index(tiny_right,{"a"},true,1);
        options.scratch_bytes=180;
        auto tl=open_h5ad(tiny_left,options),tr=open_h5ad(tiny_right,options);
        detail::compare_backed_table_indexes(tl.obs(),tr.obs());
        BackedTable tiny_targets[]{tr.obs(),tr.obs()};
        detail::compare_backed_table_indexes(tl.obs(),tiny_targets,2);
        std::cout<<"Bounded gathered index comparison passed\n";
    } catch (const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
