#pragma once

// Include after the spatialdata module declaration. These functions are private
// compiled implementation details and expose no Arrow or Parquet types.
namespace anndatapp::spatial_detail {

SpatialPoints read_parquet_points(std::string_view path, std::size_t first,
    std::size_t count, std::vector<SpatialAxis> axes,
    std::size_t scratch_bytes = 64u * 1024u * 1024u);
SpatialShapes read_parquet_shapes(std::string_view path,
    std::size_t scratch_bytes = 64u * 1024u * 1024u);
SpatialShapes read_parquet_shapes(std::string_view path, std::size_t first,
    std::size_t count, std::size_t scratch_bytes = 64u * 1024u * 1024u);
void preflight_parquet_points(std::string_view, const SpatialPointsView&, std::size_t, std::size_t);
void preflight_parquet_shapes(std::string_view, const SpatialShapesView&, std::size_t, std::size_t);
void write_parquet_points(std::string_view path, const SpatialPointsView&,
    std::size_t row_group_rows, std::size_t scratch_bytes = 64u * 1024u * 1024u);
void write_parquet_shapes(std::string_view path, const SpatialShapesView&,
    std::size_t row_group_rows, std::size_t scratch_bytes = 64u * 1024u * 1024u);

} // namespace anndatapp::spatial_detail
