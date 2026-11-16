#include <hdf5.h>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

import anndatapp;
import anndatapp.dataset;
using namespace anndatapp;

void require(bool ok,const char* text) { if (!ok) throw std::runtime_error(text); }
template<class F> void rejects(F&& f) {
    try { f(); } catch (const Error&) { return; }
    throw std::runtime_error("expected index rejection");
}
void replace_index(const std::string& path,const std::vector<std::string>& names,bool fixed,std::size_t width=48) {
    auto file=H5Fopen(path.c_str(),H5F_ACC_RDWR,H5P_DEFAULT);
    require(file>=0,"open raw string fixture");
    require(H5Ldelete(file,"/obs/_index",H5P_DEFAULT)>=0,"replace raw string fixture");
    auto type=H5Tcopy(H5T_C_S1);
    require(H5Tset_size(type,fixed?width:H5T_VARIABLE)>=0,"set string width");
    require(H5Tset_cset(type,H5T_CSET_UTF8)>=0,"set string encoding");
    hsize_t n=names.size();auto space=H5Screate_simple(1,&n,nullptr);
    auto data=H5Dcreate2(file,"/obs/_index",type,space,H5P_DEFAULT,H5P_DEFAULT,H5P_DEFAULT);
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
    H5Dclose(data);H5Sclose(space);H5Tclose(type);H5Fclose(file);
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
    DatasetOptions options;options.backed.mode=AccessMode::read_write;
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
int main(int argc,char** argv) {
    try {
        if (argc!=2) throw std::runtime_error("output directory required");
        std::filesystem::create_directories(argv[1]);
        empty_dataset_invalid_peer(argv[1]);
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
