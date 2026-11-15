#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <utility>
#include <vector>
#include <unistd.h>

import anndata;
using namespace anndatapp;
namespace {
int failures=0;
void expect(bool x,const char* message){if(!x){++failures;std::cerr<<"FAIL: "<<message<<'\n';}}
template<class F>void error(F f,const char* message){try{f();expect(false,message);}catch(const Error&){} }
template<class T>Array array(std::initializer_list<T> values,std::vector<std::size_t> shape){auto a=Array::empty<T>(std::move(shape));auto p=a.edit().template dense<T>();std::size_t i=0;for(auto x:values)p.data()[i++]=x;return a;}
SpatialPoints points(){auto t=Table::numbered(3);t.set("x",Column::from_values<double>({1,2,3}));t.set("y",Column::from_values<double>({4,5,6}));t.set("gene",Column::categorical(array<std::int8_t>({1,0,-1},{3}),Column::strings({"a","b"}),true));SpatialPoints p(std::move(t),Column::from_values<std::int64_t>({7,7,9}));p.set_feature_key("gene");p.set_transforms({{"slide",SpatialTransform::translation({SpatialAxis::x,SpatialAxis::y},{10,-2})}});return p;}
SpatialShapes polygons(){return SpatialShapes::polygons(array<double>({0,0,2,0,2,2,0,0, 5,5,6,5,6,6,5,5},{8,2}),array<std::uint64_t>({0,4,8},{3}),array<std::uint64_t>({0,1,2},{3}),array<std::uint64_t>({0,1,2},{3}),Column::strings({"p","m"}),{},array<std::uint8_t>({3,6},{2}));}
SpatialData dataset(){
    SpatialData d;SpatialRaster image(SpatialElementKind::image,{SpatialAxis::c,SpatialAxis::y,SpatialAxis::x});auto a=Array::empty<float>({1,4,4});auto v=a.edit().dense<float>();for(std::size_t i=0;i<16;++i)v.data()[i]=float(i);image.add_level(std::move(a));image.add_level(array<float>({0,2,8,10},{1,2,2}));auto labels=Element::sequence();labels.append(Element(std::int64_t(4)));image.set_channel_labels(std::move(labels));image.set_transforms({{"slide",SpatialTransform::translation({SpatialAxis::c,SpatialAxis::y,SpatialAxis::x},{0,10,20})}});d.set_raster("image",std::move(image));
    SpatialRaster odd(SpatialElementKind::image,{SpatialAxis::c,SpatialAxis::y,SpatialAxis::x});auto odd_base=Array::empty<float>({1,5,5});auto odd_child=Array::empty<float>({1,3,3});for(std::size_t i=0;i<25;++i)odd_base.edit().dense<float>().data()[i]=float(i);for(std::size_t i=0;i<9;++i)odd_child.edit().dense<float>().data()[i]=float(i);odd.add_level(std::move(odd_base));odd.add_level(std::move(odd_child));odd.set_level_transform(1,compose(SpatialTransform::translation({SpatialAxis::c,SpatialAxis::y,SpatialAxis::x},{0,.5,.5}),SpatialTransform::scale({SpatialAxis::c,SpatialAxis::y,SpatialAxis::x},{1,2,2})));d.set_raster("odd",std::move(odd));
    SpatialRaster mask(SpatialElementKind::labels,{SpatialAxis::y,SpatialAxis::x});mask.add_level(array<std::int32_t>({0,1,1,-2},{2,2}));d.set_raster("labels",std::move(mask));d.set_points("transcripts",points());d.set_shapes("polygons",polygons());d.set_shapes("circles",SpatialShapes::circles(array<double>({1,2,3,4},{2,2}),array<double>({.5,2},{2}),Column::from_values<std::int64_t>({1,2})));
    AnnData table=AnnData::empty(1,0);auto obs=Table::with_index({"row"});obs.set("region",Column::categorical(array<std::int8_t>({0},{1}),Column::strings({"missing"})));obs.set("instance",Column::from_values<std::int64_t>({42}));table.set_obs(std::move(obs));d.set_table("annotations",std::move(table),{{"missing"},"region","instance"});auto metadata=Element::mapping();metadata.set("sample",Element("β"));auto nested=Element::sequence();nested.append(Element(nullptr));nested.append(Element(std::int64_t(8)));metadata.set("nested",std::move(nested));d.set_metadata(std::move(metadata));return d;
}
template<class T>void numeric_source_rows(){
    ColumnView selected;
    {auto c=Column::from_values<T>({T(1),T(2),T(3),T(4)});selected=c.view().select(Axis::gather({3,0,2})).select(Axis::stride(2,3,1,true));}
    auto source=selected.source_values().template dense<T>();
    expect(source.extent(0)==4&&source.data()[selected.source_row(0)]==T(3)&&source.data()[selected.source_row(1)]==T(1)&&source.data()[selected.source_row(2)]==T(4),"numeric source storage and composed row maps retain exact values and lifetime");
    error([&]{selected.source_row(3);},"numeric source row checks bounds");
}
SpatialData annotation_rows(std::size_t n){
    AnnData a=AnnData::empty(n,0);auto obs=Table::numbered(n);std::vector<std::string> regions(n,"missing");obs.set("region",Column::strings(std::move(regions)));auto ids=Array::empty<std::uint64_t>({n});for(std::size_t i=0;i<n;++i)ids.edit().dense<std::uint64_t>().data()[i]=~std::uint64_t{0}-i;obs.set("instance",Column::numeric(std::move(ids)));a.set_obs(std::move(obs));SpatialData out;out.set_table("annotations",std::move(a),{{"missing"},"region","instance"});return out;
}
Array zero_coordinates(std::size_t n){auto out=Array::empty<double>({n,2});auto v=out.edit().dense<double>();for(std::size_t i=0;i<n*2;++i)v.data()[i]=0;return out;}
void validation_budget(){
    numeric_source_rows<std::int8_t>();numeric_source_rows<std::uint8_t>();numeric_source_rows<std::int16_t>();numeric_source_rows<std::uint16_t>();numeric_source_rows<std::int32_t>();numeric_source_rows<std::uint32_t>();numeric_source_rows<std::int64_t>();numeric_source_rows<std::uint64_t>();numeric_source_rows<float>();numeric_source_rows<double>();
    auto strings=Column::strings({"one"});error([&]{strings.view().source_values();},"source numeric storage rejects strings");error([&]{strings.view().source_row(0);},"source numeric rows reject strings");
    auto categorical=Column::categorical(array<std::int8_t>({0},{1}),Column::strings({"one"}));error([&]{categorical.view().source_values();},"source numeric storage rejects categoricals");
    std::uint64_t ids[]={~std::uint64_t{0},~std::uint64_t{0}-1,9007199254740993ULL,9007199254740992ULL,4,3,2,1};
    auto shapes=SpatialShapes::circles(zero_coordinates(8),array<double>({1,1,1,1,1,1,1,1},{8}),Column::numeric(Array::borrow_dense(ids+7,{8},{-1})));
    shapes.validate(64);shapes.view().validate(64);auto gather=shapes.view().select(Axis::gather({7,0,5,2,6,1,4,3}));gather.validate(64);
    error([&]{gather.validate(63);},"shape index workspace respects caller budget");expect(gather.identifiers().source_values().dense<std::uint64_t>().data()[std::ptrdiff_t(gather.identifiers().source_row(0))*-1]==~std::uint64_t{0},"failed budget validation preserves strided gathered uint64 IDs");
    auto duplicate=gather.select(Axis::gather({0,1,0}));error([&]{duplicate.validate(64);},"gathered exact duplicate identifiers are rejected");
    auto text=SpatialShapes::circles(zero_coordinates(8),array<double>({1,1,1,1,1,1,1,1},{8}),Column::strings({"a","b","c","d","e","f","g","h"}));text.validate(64);error([&]{text.validate(63);},"string shape index workspace respects caller budget");
    auto table=annotation_rows(8);table.validate(64);table.view().validate(64);error([&]{table.validate(63);},"annotation index workspace respects caller budget");error([&]{table.view().validate(63);},"annotation view index workspace respects caller budget");expect(table.view().table("annotations").n_obs()==8,"failed annotation validation preserves result");
}
void native_models(){
    validation_budget();
    auto p=points();p.validate();auto retained=p.view().select(Axis::gather({2,0,0}));p=points();auto selected_ids=retained.identifiers().values().copy();expect(selected_ids.view().dense<std::int64_t>().data()[0]==9,"point selection preserves typed IDs and ordering");expect(retained.values().column("gene").code(0)==-1,"point selection preserves categorical missingness");retained.copy().validate();
    error([&]{(void)SpatialShapes::polygons(zero_coordinates(0),array<std::uint64_t>({0},{1}),array<std::uint64_t>({0},{1}),array<std::uint64_t>({0},{1}),Column::strings({"one"}));},"geometry offset count checked before type inference");
    error([&]{(void)SpatialShapes::polygons(zero_coordinates(0),array<std::uint64_t>({0},{1}),array<std::uint64_t>({0},{1}),array<std::uint32_t>({0,0},{2}),Column::strings({"one"}));},"geometry offset dtype checked before type inference");
    error([&]{(void)SpatialShapes::polygons(zero_coordinates(0),array<std::uint64_t>({0},{1}),array<std::uint64_t>({0},{1}),array<std::uint64_t>({1,0},{2}),Column::strings({"one"}));},"geometry sentinel chain checked before type inference");
    auto bad_ids=SpatialShapes::circles(zero_coordinates(1),array<double>({1},{1}),Column::from_packed_strings({char(0xff)},{0,1},{},Validation::none));
    error([&]{bad_ids.validate();},"shape owner rejects invalid UTF-8 identifiers");error([&]{bad_ids.view().validate();},"shape view rejects invalid UTF-8 identifiers");
    auto excess_xy=SpatialShapes::circles(zero_coordinates(2),array<double>({1},{1}),Column::strings({"a"}));
    error([&]{excess_xy.validate();},"shape owner rejects excess circle coordinates");error([&]{excess_xy.view().validate();},"shape view rejects excess circle coordinates");
    auto excess_radius=SpatialShapes::circles(zero_coordinates(1),array<double>({1,2},{2}),Column::strings({"a"}));
    error([&]{excess_radius.validate();},"shape owner rejects excess circle radii");error([&]{excess_radius.view().validate();},"shape view rejects excess circle radii");
    auto excess_geometry=SpatialShapes::polygons(zero_coordinates(0),array<std::uint64_t>({0},{1}),array<std::uint64_t>({0},{1}),array<std::uint64_t>({0,0,0},{3}),Column::strings({"a"}),{},array<std::uint8_t>({6,6},{2}));
    error([&]{excess_geometry.validate();},"shape owner rejects excess polygon geometries");error([&]{excess_geometry.view().validate();},"shape view rejects excess polygon geometries");
    auto valid_circles=SpatialShapes::circles(zero_coordinates(2),array<double>({1,2},{2}),Column::strings({"a","b"}));valid_circles.view().select(Axis::gather({1})).validate();
    auto shape=polygons();shape.validate();auto selected=shape.view().select(Axis::gather({1,0}));auto packed=selected.copy();packed.validate();auto types=packed.view().geometry_types().dense<std::uint8_t>();expect(types.data()[0]==6&&types.data()[1]==3,"shape copy preserves single-part MultiPolygon type");expect(packed.view().coordinates().dense<double>().data()[0]==5,"shape selection explicitly repacks geometry buffers");
    double xy_reversed[]={1,2,3,4},r_reversed[]={.5,2};auto reversed=SpatialShapes::circles(Array::borrow_dense(xy_reversed+2,{2,2},{-2,1}),Array::borrow_dense(r_reversed+1,{2},{-1}),Column::from_values<std::int64_t>({2,1}));reversed.validate();auto reversed_copy=reversed.view().copy();reversed_copy.validate();expect(reversed_copy.view().coordinates().dense<double>().data()[0]==3&&reversed_copy.view().radii().dense<double>().data()[0]==2,"native geometry preserves signed source strides");
    auto d=dataset();d.validate();d.view().validate();auto view=d.view();d.erase("image");expect(view.contains("image")&&!d.view().contains("image"),"spatial view retains replaced structural bindings");const Axis axes[]={Axis::all(),Axis::stride(1,1),Axis::all()};auto window=view.raster("image").window(axes,3,1);expect(window.level(0).extent(1)==1&&window.channel_labels().at(0).signed_integer()==4,"raster window retains typed channels and selected values");double x[]={0,0,1},y[3]{};transform_coordinates(window.transforms()[0].transform,x,3,y,3,1);expect(y[0]==0&&y[1]==12&&y[2]==22,"pyramid crop composes level scale and spatial translation");window.copy().validate();
    const Axis all_axes[]={Axis::all(),Axis::all(),Axis::all()};auto odd_window=view.raster("odd").window(all_axes,3,1);double odd_input[]={0,1,1},odd_output[3]{};transform_coordinates(odd_window.transforms()[0].transform,odd_input,3,odd_output,3,1);expect(odd_output[1]==2.5&&odd_output[2]==2.5,"odd-sized pyramid uses stored scale2 and translation rather than extent ratio");auto odd_copy=view.raster("odd").copy();expect(odd_copy.view().level_transform(1).m[6]==2&&odd_copy.view().level_transform(1).m[8]==.5,"native copies retain exact per-level transforms");
    auto subset=view.select({"labels","annotations"}).copy();subset.validate();expect(subset.view().names().size()==2,"element selection retains annotations including valid dangling references");
    SpatialData names;auto empty=[](){SpatialRaster r(SpatialElementKind::labels,{SpatialAxis::y,SpatialAxis::x});r.add_level(array<std::uint8_t>({1},{1,1}));return r;};names.set_raster("ΟΣ",empty());error([&]{names.set_raster("ος",empty());},"Unicode default lowercase collisions are rejected");error([&]{names.set_raster("bad/name",empty());},"invalid element names are rejected");names.set_raster("細胞",empty());names.validate();
    auto invalid=Table::numbered(1);invalid.set("x",Column::from_values<std::uint16_t>({1}));invalid.set("y",Column::from_values<double>({2}));SpatialPoints bad(std::move(invalid),Column::from_values<std::int64_t>({1}));error([&]{bad.validate();},"unsupported coordinate dtypes are rejected");
}
void storage(const std::filesystem::path& path){
#if ANNDATAPP_HAS_ZARR
    const auto metadata_path=std::filesystem::path(path.string()+"-metadata");SpatialData original;write_spatialdata(metadata_path.string(),original);SpatialData bad_metadata;auto metadata=Element::mapping();metadata.set("unsupported",Element(Column::strings({"x"})));bad_metadata.set_metadata(std::move(metadata));error([&]{write_spatialdata(metadata_path.string(),bad_metadata);},"unsupported JSON metadata fails before destination creation");expect(read_spatialdata(metadata_path.string()).view().names().empty(),"metadata serialization preflight preserves existing destination");
    const auto annotation_path=std::filesystem::path(path.string()+"-budget");auto eight=annotation_rows(128);SpatialWriteOptions small_write;small_write.scratch_bytes=1024;write_spatialdata(annotation_path.string(),eight);SpatialReadOptions small_read;small_read.scratch_bytes=1024;auto accepted=read_spatialdata(annotation_path.string());accepted.validate(1024);expect(accepted.view().table("annotations").n_obs()==128,"stored annotation permits fitting 1024-byte validation index");auto nine=annotation_rows(129);error([&]{write_spatialdata(annotation_path.string(),nine,small_write);},"write preflight rejects nonfitting annotation workspace");expect(read_spatialdata(annotation_path.string()).view().table("annotations").n_obs()==128,"budget preflight preserves existing destination");write_spatialdata(annotation_path.string(),nine);error([&]{read_spatialdata(annotation_path.string(),small_read);},"eager read enforces caller scratch budget");expect(accepted.view().table("annotations").n_obs()==128,"failed eager read preserves prior returned result");
    auto data=dataset();
    if(!spatial_parquet_available()){
        {auto native=SpatialData();SpatialRaster r(SpatialElementKind::labels,{SpatialAxis::y,SpatialAxis::x});r.add_level(array<std::int32_t>({0,1},{1,2}));native.set_raster("mask",std::move(r));write_spatialdata(path.string(),native);}
        error([&]{write_spatialdata(path.string(),data);},"Parquet-off writes reject full vector payloads before truncating");auto still=read_spatialdata(path.string());expect(still.view().contains("mask"),"capability preflight preserves existing destination");return;
    }
    write_spatialdata(path.string(),data);auto backed=open_spatialdata(path.string());expect(backed.names().size()==7,"backed discovery finds all spatial elements");auto odd_transform=backed.raster_level_transform("odd",1);expect(odd_transform.m[6]==2&&odd_transform.m[8]==.5,"stored pyramid transform is retained by backed inspection");auto raw=backed.raster("image",1);float buffer[4]{};raw.read_into(DenseBuffer::from(buffer,{1,2,2}));expect(buffer[3]==10,"backed raster transfers into caller output");auto batch=backed.read_points("transcripts",1,1);expect(batch.view().identifiers().values().dense<std::int64_t>().data()[0]==7,"backed points read a bounded native batch");auto geometry_batch=backed.read_shapes("polygons",1,1);expect(geometry_batch.view().size()==1&&geometry_batch.view().geometry_types().dense<std::uint8_t>().data()[0]==6,"backed shapes expose explicit selected row batches");auto reopened=backed.read();reopened.validate();expect(reopened.view().metadata().at("nested").at(0).kind()==ElementKind::null,"nested root metadata retains null values");auto label=reopened.view().raster("image").channel_labels().at(0);expect((label.kind()==ElementKind::unsigned_integer?label.unsigned_integer():std::uint64_t(label.signed_integer()))==4,"storage preserves numeric channel labels");expect(reopened.view().points("transcripts").transforms().size()==1&&reopened.view().points("transcripts").transforms()[0].coordinate_system=="slide","storage preserves exact named transform collections");backed.close();error([&]{raw.read();},"explicit close invalidates retained raw raster handle");
#else
    (void)path;
#endif
}
}
int main(int argc,char** argv){try{
    if(argc==4&&std::string(argv[1])=="--roundtrip"){auto data=read_spatialdata(argv[2]);data.validate();write_spatialdata(argv[3],data);auto result=read_spatialdata(argv[3]);result.validate();expect(data.view().names()==result.view().names(),"reference roundtrip preserves element names");}
    else{native_models();std::cout<<"SpatialData native model tests passed\n";const auto path=argc==2?std::filesystem::path(argv[1]):std::filesystem::temp_directory_path()/("anndatapp-spatial-"+std::to_string(getpid())+".zarr");storage(path);}
}catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';++failures;}return failures?1:0;}
