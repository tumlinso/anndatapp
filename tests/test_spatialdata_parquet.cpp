module;
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>
module anndatapp.spatialdata;
#include "../src/spatialdata_parquet.hpp"
using namespace anndatapp;
namespace fs = std::filesystem;
template<class T> Array make_array(std::vector<T> values, std::vector<std::size_t> shape) {
    auto a = Array::empty<T>(std::move(shape)); auto p = a.edit().template dense<T>().data();
    for (std::size_t i = 0; i < values.size(); ++i) p[i] = values[i]; return a;
}
extern "C" int spatial_parquet_test(const char* d, const char* ext, int nrefs, const char* const* refs) {
try {
    auto dir = fs::path(d); fs::create_directories(dir);
    auto values = Table::numbered(5);
    values.set("x", Column::numeric(make_array<double>({0,1,2,3,4},{5})));
    values.set("y", Column::numeric(make_array<double>({10,11,12,13,14},{5})));
    auto codes = make_array<std::int8_t>({0,1,-1,0,1},{5});
    values.set("gene", Column::categorical(std::move(codes), Column::strings({"a","b"}), true));
    values.set("u8",Column::categorical(make_array<std::uint8_t>({0,1,0,1,0},{5}),Column::strings({"a","b"}),true));
    values.set("u16",Column::categorical(make_array<std::uint16_t>({0,1,0,1,0},{5}),Column::strings({"a","b"}),true));
    values.set("u32",Column::categorical(make_array<std::uint32_t>({0,1,0,1,0},{5}),Column::strings({"a","b"}),true));
    values.set("u64",Column::categorical(make_array<std::uint64_t>({0,1,0,1,0},{5}),Column::strings({"a","b"}),true));
    values.set("comment",Column::strings({"a","b","c","d","e"},{1,0,1,1,1}));
    auto ids = Column::numeric(make_array<std::int64_t>({90,91,92,93,94},{5}));
    SpatialPoints points(std::move(values),std::move(ids));
    auto points_path = (dir/"points.parquet").string();
    spatial_detail::write_parquet_points(points_path,points.view(),2);
    auto p = spatial_detail::read_parquet_points(points_path,1,3,{SpatialAxis::x,SpatialAxis::y}).view();
    assert(p.size()==3 && p.identifiers().values().dense<std::int64_t>().data()[0]==91);
    assert(p.values().column("x").values().dense<double>().data()[2]==3);
    for (auto name : {"u8","u16","u32","u64"}) { auto c=p.values().column(name); assert(c.ordered() && c.code(0)==1 && c.code(1)==0); }
    assert(p.values().column("gene").kind()==ColumnKind::categorical);
    assert(p.values().column("gene").ordered());
    assert(p.values().column("gene").code(0)==1 && !p.values().column("gene").valid(1));
    assert(!p.values().column("comment").valid(0));
    auto z=spatial_detail::read_parquet_points(points_path,5,0,{SpatialAxis::x,SpatialAxis::y}).view();
    assert(z.size()==0);
    bool budget_rejected = false;
    auto too_small = (dir/"insufficient-scratch.parquet").string();
    try { spatial_detail::write_parquet_points(too_small,points.view(),2,63); }
    catch (const Error& e) { budget_rejected = e.code()==ErrorCode::invalid_argument; }
    assert(budget_rejected && !fs::exists(too_small));
    auto recoded=points.view().select(Axis::gather({1,3}));
    auto recoded_path=(dir/"recoded-points.parquet").string();
    spatial_detail::write_parquet_points(recoded_path,recoded,1);
    auto recoded_result=spatial_detail::read_parquet_points(recoded_path,0,2,{SpatialAxis::x,SpatialAxis::y}).view();
    for(auto name:{"u8","u16","u32","u64"}) {auto c=recoded_result.values().column(name);assert(c.ordered() && c.categories().size()==1 && c.categories().string(0)=="b" && c.code(0)==0 && c.code(1)==0);}
    auto gathered=points.view().select(Axis::gather({4,1,3,0,2}));
    auto bounded_path=(dir/"bounded-points.parquet").string();
    spatial_detail::write_parquet_points(bounded_path,gathered,65536,64);
    auto bounded=spatial_detail::read_parquet_points(bounded_path,0,5,{SpatialAxis::x,SpatialAxis::y},64).view();
    assert(bounded.identifiers().values().dense<std::int64_t>().data()[0]==94);
    auto xy=make_array<double>({1,2,3,4},{2,2});auto radii=make_array<double>({1,2},{2});
    auto circles=SpatialShapes::circles(std::move(xy),std::move(radii),Column::strings({"a","b"}));
    auto circles_path=(dir/"circles.parquet").string();
    spatial_detail::write_parquet_shapes(circles_path,circles.view(),1);
    auto cs=spatial_detail::read_parquet_shapes(circles_path).view();
    assert(cs.size()==2 && cs.kind()==SpatialGeometryKind::circles);
    auto cs_one = spatial_detail::read_parquet_shapes(circles_path, 1, 1, 64).view();
    assert(cs_one.size()==1 && cs_one.radii().dense<double>().data()[0] == 2);
    auto cs_empty = spatial_detail::read_parquet_shapes(circles_path, 2, 0, 64).view();
    assert(cs_empty.size()==0 && cs_empty.kind()==SpatialGeometryKind::circles);
    assert(cs.coordinates().dense<double>().data()[3]==4 && cs.radii().dense<double>().data()[1]==2);
    assert(cs.identifiers().string(0)=="a" && cs.identifiers().string(1)=="b");
    auto px=make_array<double>({0,0,2,0,2,2,0,0, 3,3,4,3,4,4,3,3},{8,2});
    auto ro=make_array<std::uint64_t>({0,4,8},{3});
    auto po=make_array<std::uint64_t>({0,1,2},{3});
    auto go=make_array<std::uint64_t>({0,1,2},{3});
    auto ty=make_array<std::uint8_t>({3,6},{2});
    auto polygons=SpatialShapes::polygons(std::move(px),std::move(ro),std::move(po),std::move(go),Column::from_values<std::int64_t>({7,8}),{},std::move(ty));
    auto path=(dir/"shapes.parquet").string();
    spatial_detail::write_parquet_shapes(path,polygons.view(),1);
    auto sh=spatial_detail::read_parquet_shapes(path).view();
    auto sh_one=spatial_detail::read_parquet_shapes(path,1,1,64).view();
    assert(sh_one.size()==1 && sh_one.geometry_types().dense<std::uint8_t>().data()[0]==6);
    auto sh_empty=spatial_detail::read_parquet_shapes(path,2,0,64).view();
    assert(sh_empty.size()==0 && sh_empty.kind()==SpatialGeometryKind::polygons);
    assert(sh.size()==2 && sh.kind()==SpatialGeometryKind::polygons);
    assert(sh.geometry_types().dense<std::uint8_t>().data()[1]==6);
    assert(sh.identifiers().values().dense<std::int64_t>().data()[0]==7);
    auto external=ext && *ext ? fs::path(ext) : fs::path();
    if (!external.empty()) {
    auto ep=spatial_detail::read_parquet_points((external/"points.parquet").string(),1,4,{SpatialAxis::x,SpatialAxis::y}).view();
    auto eid=ep.identifiers().values().dense<std::int64_t>();
    assert(ep.size()==4 && eid.data()[0]==101 && eid.data()[1]==102 && eid.data()[2]==200 && eid.data()[3]==201);
    assert(ep.values().index_name()=="rowid");
    auto range=spatial_detail::read_parquet_shapes((external/"unselected-invalid-wkb.parquet").string(),1,1,64).view();
    assert(range.kind()==SpatialGeometryKind::circles && range.size()==1 && range.radii().dense<double>().data()[0]==2);
    auto empty_range=spatial_detail::read_parquet_shapes((external/"unselected-invalid-wkb.parquet").string(),0,0,64).view();
    assert(empty_range.kind()==SpatialGeometryKind::circles && empty_range.size()==0);
    auto es=spatial_detail::read_parquet_shapes((external/"point.parquet").string()).view();
    assert(es.kind()==SpatialGeometryKind::circles && es.radii().dense<double>().data()[1]==2);
    es=spatial_detail::read_parquet_shapes((external/"polygon.parquet").string()).view();
    assert(es.kind()==SpatialGeometryKind::polygons && es.geometry_types().dense<std::uint8_t>().data()[1]==3);
    es=spatial_detail::read_parquet_shapes((external/"multipolygon.parquet").string()).view();
    assert(es.kind()==SpatialGeometryKind::polygons && es.geometry_types().dense<std::uint8_t>().data()[1]==6);
    }
    for (int i=0; i<nrefs; ++i) {
        auto fixture=fs::path(refs[i]);
        auto actual_points=spatial_detail::read_parquet_points((fixture/"points/points/points.parquet").string(),0,2,{SpatialAxis::x,SpatialAxis::y}).view();
        assert(actual_points.size()==2);
        if (i<2) {
            auto cells=fixture/"shapes/cells/shapes.parquet";
            auto polygons=fixture/"shapes/polygon_with_hole/shapes.parquet";
            auto actual_shapes=spatial_detail::read_parquet_shapes((fs::exists(cells)?cells:polygons).string()).view();
            assert(actual_shapes.size()>0 && actual_shapes.kind()==SpatialGeometryKind::polygons);
        } else {
            auto circles_ref=spatial_detail::read_parquet_shapes((fixture/"shapes/circles/shapes.parquet").string()).view();
            auto polygons_ref=spatial_detail::read_parquet_shapes((fixture/"shapes/polygons/shapes.parquet").string()).view();
            auto multi_ref=spatial_detail::read_parquet_shapes((fixture/"shapes/multipolygons/shapes.parquet").string()).view();
            assert(circles_ref.size()==2 && circles_ref.kind()==SpatialGeometryKind::circles);
            assert(polygons_ref.size()==1 && polygons_ref.geometry_types().dense<std::uint8_t>().data()[0]==3);
            assert(multi_ref.size()==2 && multi_ref.geometry_types().dense<std::uint8_t>().data()[0]==6);
        }
    }
    auto geo=external/"geopandas";
    if (!external.empty() && fs::exists(geo/"point.parquet")) {
    auto gc=spatial_detail::read_parquet_shapes((geo/"point.parquet").string()).view();
    assert(gc.size()==2 && gc.radii().dense<double>().data()[1]==2);
    auto gp=spatial_detail::read_parquet_shapes((geo/"polygon.parquet").string()).view();
    assert(gp.size()==2 && gp.geometry_types().dense<std::uint8_t>().data()[1]==3);
    auto gm=spatial_detail::read_parquet_shapes((geo/"multipolygon.parquet").string()).view();
    assert(gm.size()==2 && gm.geometry_types().dense<std::uint8_t>().data()[1]==6);
    }
    if (!external.empty()) {
    auto di=spatial_detail::read_parquet_points((external/"dictionary_index.parquet").string(),0,2,{SpatialAxis::x,SpatialAxis::y}).view();
    assert(di.identifiers().kind()==ColumnKind::string && di.identifiers().string(1)=="cell-b");
    auto di0=spatial_detail::read_parquet_points((external/"dictionary_index.parquet").string(),2,0,{SpatialAxis::x,SpatialAxis::y}).view();
    assert(di0.size()==0 && di0.identifiers().kind()==ColumnKind::string);
    }
    auto selected=polygons.view().select(Axis::gather({1,0}));
    spatial_detail::write_parquet_shapes((dir/"reordered.parquet").string(),selected,1);
    auto reordered=spatial_detail::read_parquet_shapes((dir/"reordered.parquet").string()).view();
    assert(reordered.identifiers().values().dense<std::int64_t>().data()[0]==8);
    assert(reordered.geometry_types().dense<std::uint8_t>().data()[0]==6);
    auto one=polygons.view().select(Axis::gather({0}));
    spatial_detail::write_parquet_shapes((dir/"polygon_only.parquet").string(),one,1);
    auto bounded_shapes=(dir/"bounded-shapes.parquet").string();
    spatial_detail::write_parquet_shapes(bounded_shapes,polygons.view(),65536,64);
    assert(spatial_detail::read_parquet_shapes(bounded_shapes,64).view().size()==2);
    std::cout << "SpatialData Parquet native + independent references passed\n"; return 0;
} catch(const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
