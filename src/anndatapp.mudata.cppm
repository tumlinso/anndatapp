module;
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

export module anndatapp.mudata;
export import anndatapp.backed;

export namespace anndatapp {

enum class MuAxis : int { both = -1, observations = 0, variables = 1 };

// Map values are one-based positions in a modality; zero means absent.
// The pointer remains valid while the originating MuDataView or BackedMuDataView remains alive.
struct MuMapView {
    const std::uint64_t* data = nullptr;
    std::size_t size = 0;
};

namespace detail { struct MuDataImpl; struct MuDataViewImpl; struct BackedMuDataImpl; struct BackedMuDataViewImpl; }
class MuDataView;

class MuData {
public:
    MuData();
    ~MuData();
    MuData(MuData&&) noexcept;
    MuData& operator=(MuData&&) noexcept;
    MuData(const MuData&) = delete;
    MuData& operator=(const MuData&) = delete;
    static MuData empty(std::size_t n_obs, std::size_t n_vars,
                        MuAxis axis = MuAxis::observations);
    MuAxis axis() const;
    AnnData& annotations();
    const AnnData& annotations() const;
    // Replacing the edited modality requires explicitly updating its maps.
    AnnData& edit_modality(std::string_view name);
    void set_maps(std::string_view name, std::vector<std::uint64_t> obs_map,
                  std::vector<std::uint64_t> var_map);
    // Maps must match the global axis lengths and cover the modality's axes.
    void set_modality(std::string name, AnnData value,
                      std::vector<std::uint64_t> obs_map,
                      std::vector<std::uint64_t> var_map);
    void erase_modality(std::string_view name);
    // Retains AnnData views and shares immutable maps without scanning or copying map payloads.
    MuDataView view() const;
    // Visits selected global positions for each modality axis. Allocates a projected
    // map and compact local-position list per axis, then retains AnnData slice views.
    MuDataView slice(Selection obs = all, Selection var = all) const;
    MuData copy() const;
    void validate() const;
private:
    std::unique_ptr<detail::MuDataImpl> impl_;
};

class MuDataView {
public:
    MuDataView();
    MuAxis axis() const;
    std::size_t n_obs() const;
    std::size_t n_vars() const;
    AnnDataView annotations() const;
    std::vector<std::string> modality_names() const;
    AnnDataView modality(std::string_view name) const;
    MuMapView obs_map(std::string_view name) const;
    MuMapView var_map(std::string_view name) const;
    MuDataView slice(Selection obs = all, Selection var = all) const;
    MuData copy() const;
private:
    std::shared_ptr<const detail::MuDataViewImpl> impl_;
    explicit MuDataView(std::shared_ptr<const detail::MuDataViewImpl>);
    friend class MuData;
};

class BackedMuDataView;

class BackedMuData {
public:
    BackedMuData();
    ~BackedMuData();
    BackedMuData(BackedMuData&&) noexcept;
    BackedMuData& operator=(BackedMuData&&) noexcept;
    BackedMuData(const BackedMuData&) = delete;
    BackedMuData& operator=(const BackedMuData&) = delete;
    bool is_open() const noexcept;
    MuAxis axis() const;
    std::size_t n_obs() const;
    std::size_t n_vars() const;
    std::vector<std::string> modality_names() const;
    BackedTable obs() const;
    BackedTable var() const;
    BackedElement obsm(std::string_view name) const;
    BackedElement varm(std::string_view name) const;
    BackedArray obsp(std::string_view name) const;
    BackedArray varp(std::string_view name) const;
    BackedElement uns() const;
    BackedArray obs_map(std::string_view name) const;
    BackedArray var_map(std::string_view name) const;
    BackedAnnData modality(std::string_view name) const;
    // The two map writes are synchronous, not a cross-path transaction.
    void set_maps(std::string_view name, std::vector<std::uint64_t> obs_map,
                  std::vector<std::uint64_t> var_map);
    // Retains store-backed descriptors. Positional slices defer map and matrix
    // payload reads; label slices read axis names to resolve labels.
    BackedMuDataView view() const;
    BackedMuDataView slice(Selection obs = all, Selection var = all) const;
    MuData read() const;
    void flush();
    void close();
private:
    std::shared_ptr<detail::BackedMuDataImpl> impl_;
    explicit BackedMuData(std::shared_ptr<detail::BackedMuDataImpl>);
    friend BackedMuData open_h5mu(std::string_view, BackedOptions);
    friend BackedMuData open_mudata_zarr(std::string_view, BackedOptions);
};


// Retains the open store owner, map descriptors and revisions, and positional
// selections. Positional slice creation defers map and matrix payload reads. The
// first map or modality access reads and caches each selected map; a projected
// slice also allocates a projected map and local-position list. Map pointers remain
// valid for this view lifetime. Map or modality structural mutation invalidates
// existing views. Label slices read axis names. read() materializes the selected view.
class BackedMuDataView {
public:
    BackedMuDataView();
    MuAxis axis() const;
    std::size_t n_obs() const;
    std::size_t n_vars() const;
    BackedTable obs() const;
    BackedTable var() const;
    std::vector<std::string> modality_names() const;
    BackedAnnDataView modality(std::string_view name) const;
    MuMapView obs_map(std::string_view name) const;
    MuMapView var_map(std::string_view name) const;
    BackedArray obsm_array(std::string_view name) const;
    BackedStringArray obsm_strings(std::string_view name) const;
    BackedTable obsm_table(std::string_view name) const;
    BackedArray varm_array(std::string_view name) const;
    BackedStringArray varm_strings(std::string_view name) const;
    BackedTable varm_table(std::string_view name) const;
    BackedArray obsp(std::string_view name) const;
    BackedArray varp(std::string_view name) const;
    BackedElement uns() const;
    BackedMuDataView slice(Selection obs = all, Selection var = all) const;
    MuData read() const;
private:
    std::shared_ptr<const detail::BackedMuDataViewImpl> impl_;
    explicit BackedMuDataView(std::shared_ptr<const detail::BackedMuDataViewImpl>);
    friend class BackedMuData;
};

struct MuWriteOptions {
    Validation validation = Validation::full;
    bool overwrite = false;
    std::size_t scratch_bytes = 64u * 1024u * 1024u;
};
[[nodiscard]] BackedMuData open_h5mu(std::string_view path, BackedOptions = {});
[[nodiscard]] BackedMuData open_mudata_zarr(std::string_view path, BackedOptions = {});
[[nodiscard]] MuData read_h5mu(std::string_view path, BackedOptions = {});
[[nodiscard]] MuData read_mudata_zarr(std::string_view path, BackedOptions = {});
void write_h5mu(std::string_view path, const MuDataView&, MuWriteOptions = {});
void write_h5mu(std::string_view path, const MuData&, MuWriteOptions = {});
void write_mudata_zarr(std::string_view path, const MuDataView&, MuWriteOptions = {});
void write_mudata_zarr(std::string_view path, const MuData&, MuWriteOptions = {});

} // namespace anndatapp
