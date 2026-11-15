import json, pathlib, struct, subprocess, sys
import pyarrow as pa
import pyarrow.parquet as pq

root = pathlib.Path(sys.argv[2]) / 'external'
(root / 'points.parquet').mkdir(parents=True, exist_ok=True)
for part, start in [(0, 100), (10, 200)]:
    values = pa.array([float(start),float(start+1),float(start+2)], type=pa.float64())
    fields=[pa.field('x',pa.float64()),pa.field('y',pa.float64())]
    meta={'index_columns':[{'kind':'range','name':'rowid','start':start,'step':1,'stop':start+3}], 'columns':[], 'column_indexes':[]}
    schema=pa.schema(fields, metadata={b'pandas':json.dumps(meta).encode()})
    pq.write_table(pa.Table.from_arrays([values,values],schema=schema), root/'points.parquet'/f'part.{part}.parquet', row_group_size=1)

def shape_meta(encoding, geometry_type):
    pandas={'index_columns':['__index_level_0__'],'columns':[], 'column_indexes':[]}
    geo={'version':'1.1.0','primary_column':'geometry','columns':{'geometry':{'encoding':encoding,'geometry_types':[geometry_type],'crs':None}}}
    return {b'pandas':json.dumps(pandas).encode(),b'geo':json.dumps(geo).encode()}

def write(name, encoding, geometry_type, arr, radius=None):
    arrays=[arr, pa.array([41,42],type=pa.int64())]
    names=['geometry','__index_level_0__']
    if radius is not None: arrays.append(pa.array(radius)); names.append('radius')
    t=pa.Table.from_arrays(arrays,names=names).replace_schema_metadata(shape_meta(encoding,geometry_type))
    pq.write_table(t,root/name,row_group_size=1)

point=pa.struct([('x',pa.float64()),('y',pa.float64())])
write('point.parquet','point','Point',pa.array([{'x':1.0,'y':2.0},{'x':3.0,'y':4.0}],type=point), [1.0,2.0])
write('unselected-invalid-wkb.parquet','WKB','Point',pa.array([b'bad', struct.pack('<BIdd', 1, 1, 3., 4.)],type=pa.binary()), [1.0,2.0])
ring0=[{'x':0.,'y':0.},{'x':1.,'y':0.},{'x':1.,'y':1.},{'x':0.,'y':0.}]
ring1=[{'x':2.,'y':2.},{'x':3.,'y':2.},{'x':3.,'y':3.},{'x':2.,'y':2.}]
poly=pa.list_(pa.list_(point))
write('polygon.parquet','polygon','Polygon',pa.array([[ring0],[ring1]],type=poly))
multi=pa.list_(poly)
write('multipolygon.parquet','multipolygon','MultiPolygon',pa.array([[[ring0]],[[ring1]]],type=multi))
ids = pa.array(['cell-a', 'cell-b']).dictionary_encode()
index_meta = {'index_columns':['idx'], 'columns':[], 'column_indexes':[]}
id_schema = pa.schema([pa.field('x',pa.float64()), pa.field('y',pa.float64()), pa.field('idx',ids.type)],
                      metadata={b'pandas':json.dumps(index_meta).encode()})
pq.write_table(pa.Table.from_arrays([pa.array([0.,1.]),pa.array([2.,3.]),ids],schema=id_schema),
               root/'dictionary_index.parquet')
try:
    import geopandas as gpd
    from shapely.geometry import Point, Polygon, MultiPolygon
except ImportError:
    gpd = None
if gpd is not None:
    geo = root/'geopandas'
    geo.mkdir(exist_ok=True)
    p1 = Polygon([(0,0),(1,0),(1,1),(0,0)])
    p2 = Polygon([(2,2),(3,2),(3,3),(2,2)])
    for name, geometries, columns in [
        ('point',[Point(1,2),Point(3,4)],{'radius':[1.,2.]}),
        ('polygon',[p1,p2],{}),
        ('multipolygon',[MultiPolygon([p1]),MultiPolygon([p2])],{}),
    ]:
        gpd.GeoDataFrame(columns, geometry=geometries, index=[41,42]).to_parquet(
            geo/(name+'.parquet'), geometry_encoding='geoarrow', schema_version='1.1.0')
output = pathlib.Path(sys.argv[2]) / 'output'
legacy = sys.argv[3:]
if len(legacy) not in (0,4):
    raise ValueError('Provide either zero or four reference fixture paths')
subprocess.run([sys.argv[1], str(output), str(root), *legacy], check=True)
p = pq.read_table(output / 'points.parquet')
assert p.schema.metadata and json.loads(p.schema.metadata[b'pandas'])['index_columns'] == ['__index_level_0__']
assert p['__index_level_0__'].to_pylist() == [90, 91, 92, 93, 94]
assert pa.types.is_dictionary(p['gene'].type) and p['gene'].type.ordered
assert p['gene'].to_pylist() == ['a','b',None,'a','b']
for name in ('u8', 'u16', 'u32', 'u64'):
    assert pa.types.is_dictionary(p[name].type) and p[name].type.ordered
    assert p[name].to_pylist() == ['a', 'b', 'a', 'b', 'a']
selected = pq.read_table(output / 'recoded-points.parquet')
for name in ('u8', 'u16', 'u32', 'u64'):
    assert selected[name].type.ordered and selected[name].to_pylist() == ['b', 'b']
    assert selected[name].chunk(0).dictionary.to_pylist() == ['b']
for name, types in [('circles.parquet',[1,1]), ('shapes.parquet',[3,6])]:
    t = pq.read_table(output / name)
    geo = json.loads(t.schema.metadata[b'geo'])
    assert geo['primary_column'] == 'geometry' and geo['columns']['geometry']['encoding'] == 'WKB'
    assert [struct.unpack_from('<I', b, 1)[0] for b in t['geometry'].to_pylist()] == types
only = pq.read_table(output/'polygon_only.parquet')
assert json.loads(only.schema.metadata[b'geo'])['columns']['geometry']['geometry_types'] == ['Polygon']
if gpd is not None:
    circle = gpd.read_parquet(output/'circles.parquet')
    shapes = gpd.read_parquet(output/'shapes.parquet')
    assert list(circle.index) == ['a','b'] and list(circle.geom_type) == ['Point','Point']
    assert list(shapes.index) == [7,8] and list(shapes.geom_type) == ['Polygon','MultiPolygon']
print('Independent PyArrow' + (' and GeoPandas' if gpd is not None else '') + ' spatial Parquet verification passed')
