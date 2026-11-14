module;
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

export module anndatapp.spatialdata;
export import anndatapp.backed;

export namespace anndatapp {

enum class SpatialAxis : std::uint8_t { x, y, z, c };
enum class SpatialElementKind { image, labels, points, shapes, table };
enum class SpatialGeometryKind { circles, polygons };

// A small affine map on named axes. Entries use a fixed row pitch of five;
// m[5*i + input_rank] is the translation for output axis i. No heap storage.
struct SpatialTransform {
    std::size_t input_rank = 0, output_rank = 0;
    SpatialAxis input_axes[4]{}, output_axes[4]{};
    double m[25]{};
    static SpatialTransform identity(std::initializer_list<SpatialAxis> axes);
    static SpatialTransform affine(const SpatialAxis* input, std::size_t ni,
        const SpatialAxis* output, std::size_t no, const double* matrix,
        std::ptrdiff_t row_stride);
    static SpatialTransform translation(std::initializer_list<SpatialAxis> axes,
        std::initializer_list<double> offsets);
    static SpatialTransform scale(std::initializer_list<SpatialAxis> axes,
        std::initializer_list<double> factors);
    void validate() const;
    SpatialTransform inverse() const;
};
// after(before(p)); intermediate axes are matched by name, not position.
SpatialTransform compose(const SpatialTransform& after, const SpatialTransform& before);
// Strides count doubles. Input/output do not overlap. Numerical loops allocate
// nothing; transform validation occurs once before the first coordinate.
void transform_coordinates(const SpatialTransform&, const double* input,
    std::ptrdiff_t input_row_stride, double* output,
    std::ptrdiff_t output_row_stride, std::size_t count);

struct SpatialNamedTransform {
    std::string coordinate_system;
    SpatialTransform transform;
};
struct SpatialAnnotation {
    std::vector<std::string> regions;
    std::string region_key, instance_key;
};

namespace detail {
struct SpatialRasterImpl; struct SpatialRasterViewImpl;
struct SpatialPointsImpl; struct SpatialPointsViewImpl;
struct SpatialShapesImpl; struct SpatialShapesViewImpl;
struct SpatialDataImpl; struct SpatialDataViewImpl;
struct BackedSpatialDataImpl;
}
class SpatialRasterView;
class SpatialRaster {
public:
    SpatialRaster(SpatialElementKind kind, std::vector<SpatialAxis> axes);
    ~SpatialRaster();
    SpatialRaster(SpatialRaster&&) noexcept;
    SpatialRaster& operator=(SpatialRaster&&) noexcept;
    SpatialRaster(const SpatialRaster&) = delete;
    SpatialRaster& operator=(const SpatialRaster&) = delete;
    void add_level(Array, std::string path = {});
    void set_level_transform(std::size_t level, SpatialTransform);
    void set_channels(std::vector<std::string>);
    void set_channel_labels(Element labels);
    void set_transform(std::string coordinate_system, SpatialTransform);
    void set_transforms(std::vector<SpatialNamedTransform>);
    SpatialRasterView view() const;
    SpatialRaster copy() const;
    void validate() const;
private:
    std::unique_ptr<detail::SpatialRasterImpl> impl_;
    friend class SpatialRasterView;
};
class SpatialRasterView {
public:
    SpatialRasterView();
    SpatialElementKind kind() const;
    const std::vector<SpatialAxis>& axes() const;
    std::size_t n_levels() const;
    ArrayView level(std::size_t) const;
    std::string_view level_path(std::size_t) const;
    SpatialTransform level_transform(std::size_t) const;
    const std::vector<std::string>& channels() const;
    ElementView channel_labels() const;
    const std::vector<SpatialNamedTransform>& transforms() const;
    SpatialRasterView window(const Axis* axes, std::size_t rank, std::size_t level = 0) const;
    SpatialRaster copy() const;
private:
    std::shared_ptr<detail::SpatialRasterViewImpl> impl_;
    explicit SpatialRasterView(std::shared_ptr<detail::SpatialRasterViewImpl>);
    friend class SpatialRaster;
};

class SpatialPointsView;
class SpatialPoints {
public:
    SpatialPoints(Table values, Column identifiers,
        std::vector<SpatialAxis> axes = {SpatialAxis::x, SpatialAxis::y});
    ~SpatialPoints();
    SpatialPoints(SpatialPoints&&) noexcept;
    SpatialPoints& operator=(SpatialPoints&&) noexcept;
    SpatialPoints(const SpatialPoints&) = delete;
    SpatialPoints& operator=(const SpatialPoints&) = delete;
    void set_feature_key(std::string);
    void set_instance_key(std::string);
    void set_transform(std::string coordinate_system, SpatialTransform);
    void set_transforms(std::vector<SpatialNamedTransform>);
    SpatialPointsView view() const;
    SpatialPoints copy() const;
    void validate() const;
private:
    std::unique_ptr<detail::SpatialPointsImpl> impl_;
    friend class SpatialPointsView;
};
class SpatialPointsView {
public:
    SpatialPointsView();
    std::size_t size() const;
    TableView values() const;
    ColumnView identifiers() const;
    const std::vector<SpatialAxis>& axes() const;
    std::string_view feature_key() const;
    std::string_view instance_key() const;
    const std::vector<SpatialNamedTransform>& transforms() const;
    SpatialPointsView select(const Axis&) const;
    SpatialPoints copy() const;
private:
    std::shared_ptr<detail::SpatialPointsViewImpl> impl_;
    explicit SpatialPointsView(std::shared_ptr<detail::SpatialPointsViewImpl>);
    friend class SpatialPoints;
};

class SpatialShapesView;
class SpatialShapes {
public:
    static SpatialShapes circles(Array coordinates, Array radii, Column identifiers,
        Table attributes = {});
    // Coordinates are FP64 [n,2]. Offsets are uint64 arrays, including a final
    // sentinel: ring -> coordinates, polygon -> rings, geometry -> polygons.
    static SpatialShapes polygons(Array coordinates, Array ring_offsets,
        Array polygon_offsets, Array geometry_offsets, Column identifiers,
        Table attributes = {}, Array geometry_types = {});
    ~SpatialShapes();
    SpatialShapes(SpatialShapes&&) noexcept;
    SpatialShapes& operator=(SpatialShapes&&) noexcept;
    SpatialShapes(const SpatialShapes&) = delete;
    SpatialShapes& operator=(const SpatialShapes&) = delete;
    void set_transform(std::string coordinate_system, SpatialTransform);
    void set_transforms(std::vector<SpatialNamedTransform>);
    SpatialShapesView view() const;
    SpatialShapes copy() const;
    // Duplicate-index workspace is bounded when supplied; standalone default is unlimited.
    void validate(std::size_t scratch_bytes = ~std::size_t{0}) const;
private:
    SpatialShapes();
    std::unique_ptr<detail::SpatialShapesImpl> impl_;
    friend class SpatialShapesView;
};
class SpatialShapesView {
public:
    SpatialShapesView();
    SpatialGeometryKind kind() const;
    std::size_t size() const;
    ColumnView identifiers() const;
    TableView attributes() const;
    // Topology buffers describe the retained source. source_row(i) maps a
    // selected logical geometry into them; copy() explicitly repacks buffers.
    ArrayView coordinates() const;
    ArrayView radii() const;
    ArrayView ring_offsets() const;
    ArrayView polygon_offsets() const;
    ArrayView geometry_offsets() const;
    // uint8 WKB types (3=Polygon, 6=MultiPolygon), retaining single-part multis.
    ArrayView geometry_types() const;
    std::size_t source_row(std::size_t i) const;
    const std::vector<SpatialNamedTransform>& transforms() const;
    SpatialShapesView select(const Axis&) const;
    SpatialShapes copy() const;
    // Duplicate-index workspace only; retained payload is not copied.
    void validate(std::size_t scratch_bytes = ~std::size_t{0}) const;
private:
    std::shared_ptr<detail::SpatialShapesViewImpl> impl_;
    explicit SpatialShapesView(std::shared_ptr<detail::SpatialShapesViewImpl>);
    friend class SpatialShapes;
};

class SpatialDataView;
class SpatialData {
public:
    SpatialData();
    ~SpatialData();
    SpatialData(SpatialData&&) noexcept;
    SpatialData& operator=(SpatialData&&) noexcept;
    SpatialData(const SpatialData&) = delete;
    SpatialData& operator=(const SpatialData&) = delete;
    void set_raster(std::string, SpatialRaster);
    void set_points(std::string, SpatialPoints);
    void set_shapes(std::string, SpatialShapes);
    void set_table(std::string, AnnData, SpatialAnnotation = {});
    void set_metadata(Element);
    void erase(std::string_view);
    SpatialDataView view() const;
    SpatialData copy() const;
    // Duplicate-index workspace is bounded when supplied; standalone default is unlimited.
    void validate(std::size_t scratch_bytes = ~std::size_t{0}) const;
private:
    std::unique_ptr<detail::SpatialDataImpl> impl_;
};
class SpatialDataView {
public:
    SpatialDataView();
    std::vector<std::string> names() const;
    bool contains(std::string_view) const;
    SpatialElementKind kind(std::string_view) const;
    SpatialRasterView raster(std::string_view) const;
    SpatialPointsView points(std::string_view) const;
    SpatialShapesView shapes(std::string_view) const;
    AnnDataView table(std::string_view) const;
    SpatialAnnotation annotation(std::string_view) const;
    ElementView metadata() const;
    SpatialDataView select(const std::vector<std::string>& names) const;
    SpatialData copy() const;
    // Duplicate-index workspace is bounded when supplied; standalone default is unlimited.
    void validate(std::size_t scratch_bytes = ~std::size_t{0}) const;
private:
    std::shared_ptr<detail::SpatialDataViewImpl> impl_;
    explicit SpatialDataView(std::shared_ptr<detail::SpatialDataViewImpl>);
    friend class SpatialData;
};

struct SpatialReadOptions {
    Validation validation = Validation::full;
    std::size_t scratch_bytes = 64u * 1024u * 1024u;
};
struct SpatialWriteOptions {
    Validation validation = Validation::full;
    bool exclusive = false;
    std::size_t scratch_bytes = 64u * 1024u * 1024u;
    std::size_t parquet_row_group_rows = 65536;
};
class BackedSpatialData {
public:
    BackedSpatialData();
    ~BackedSpatialData();
    BackedSpatialData(BackedSpatialData&&) noexcept;
    BackedSpatialData& operator=(BackedSpatialData&&) noexcept;
    BackedSpatialData(const BackedSpatialData&) = delete;
    BackedSpatialData& operator=(const BackedSpatialData&) = delete;
    std::vector<std::string> names() const;
    SpatialElementKind kind(std::string_view) const;
    std::vector<std::string> raster_level_paths(std::string_view) const;
    SpatialTransform raster_level_transform(std::string_view, std::size_t level) const;
    std::vector<SpatialAxis> axes(std::string_view) const;
    std::vector<SpatialNamedTransform> transforms(std::string_view) const;
    SpatialAnnotation annotation(std::string_view table) const;
    BackedArray raster(std::string_view, std::size_t level = 0) const;
    BackedAnnData table(std::string_view) const;
    // Vector payload reads are explicit row batches with owning native output;
    // no fake contiguous pointer is exposed for variable-size geometry.
    SpatialPoints read_points(std::string_view, std::size_t first,
        std::size_t count) const;
    SpatialShapes read_shapes(std::string_view, std::size_t first = 0,
        std::size_t count = ~std::size_t{0}) const;
    SpatialData read() const;
    void close();
private:
    std::unique_ptr<detail::BackedSpatialDataImpl> impl_;
    explicit BackedSpatialData(std::unique_ptr<detail::BackedSpatialDataImpl>);
    friend BackedSpatialData open_spatialdata(std::string_view, BackedOptions);
};
bool spatial_parquet_available() noexcept;
SpatialData read_spatialdata(std::string_view, SpatialReadOptions = {});
BackedSpatialData open_spatialdata(std::string_view, BackedOptions = {});
void write_spatialdata(std::string_view, const SpatialDataView&, SpatialWriteOptions = {});
void write_spatialdata(std::string_view, const SpatialData&, SpatialWriteOptions = {});

} // namespace anndatapp
