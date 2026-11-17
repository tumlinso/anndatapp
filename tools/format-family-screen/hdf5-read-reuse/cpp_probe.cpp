// Reuse the existing diagnostic observer, aligned buffers, and hash convention.
#define main collection_reference_main
#include "../collection-fix/cpp_probe.cpp"
#undef main
#include <fstream>
import anndatapp.h5ad;

namespace {
Selection selector(const std::vector<std::ptrdiff_t>& p, std::size_t extent, bool gather) {
    if (gather) return indices(p);
    require(!p.empty(), "empty range in case specification");
    for (std::size_t i = 1; i < p.size(); ++i)
        require(p[i] == p[i-1]+1, "nonconsecutive range in case specification");
    if (p.front() == 0 && p.size() == extent) return all;
    return range{p.front(), p.back()+1, 1};
}

std::vector<std::ptrdiff_t> positions(std::istream& in) {
    std::size_t size = 0;
    in >> size;
    std::vector<std::ptrdiff_t> out(size);
    for (auto& x : out) in >> x;
    require(bool(in) && !out.empty(), "invalid case positions");
    return out;
}

void metadata_copy(const Observer& observer) {
    auto data = AnnData::empty(100000, 3);
    auto view = data.view();
    observer.start(); const auto start = Clock::now();
    auto out = view.copy();
    const auto end = Clock::now(); const auto diag = observer.stop_phase();
    require(out.n_obs() == 100000 && out.n_vars() == 3 && !out.has_x(), "incorrect metadata copy shape");
    const auto obs = out.obs(), var = out.var();
    auto hash = kFnvOffset;
    for (const auto* axis : {&obs, &var})
        for (std::size_t i = 0; i < axis->size(); ++i) {
            const auto name = axis->name(i);
            require(name == std::to_string(i), "incorrect axis name after copy");
            for (unsigned char c : name) hash_byte(hash, c);
            hash_byte(hash, 0);
        }
    std::cout << "{\"case\":\"axis-copy\",\"golden\":\"pass\",\"nnz\":0"
              << ",\"payload_bytes\":0,\"output_fnv1a64\":\"" << hex64(hash) << "\""
              << ",\"preparation_ns\":0,\"transfer_ns\":" << ns(start,end)
              << ",\"operation_ns\":" << ns(start,end);
    if (observer.enabled) { std::cout << ",\"transfer_diag\":"; print_observation(diag); }
    std::cout << "}\n";
}
}

int main(int argc, char** argv) {
    try {
        require(argc == 2 || (argc == 3 && std::string_view(argv[2]) == "--diagnostic"),
                "usage: hdf5_read_reuse CASE.txt|axis-copy [--diagnostic]");
        Observer observer(argc == 3);
        if (std::string_view(argv[1]) == "axis-copy") { metadata_copy(observer); return 0; }

        std::ifstream in(argv[1]);
        std::string name, path, layer, mode, expected;
        std::getline(in, name); std::getline(in, path);
        std::getline(in, layer); std::getline(in, mode); std::getline(in, expected);
        int gather_rows = 0, gather_cols = 0;
        in >> gather_rows >> gather_cols;
        auto rows = positions(in), cols = positions(in);
        auto store = open_h5ad(path);
        auto x = layer.empty() ? store.x() : store.layer(layer);
        auto picked = x.select({selector(rows, store.n_obs(), gather_rows),
                                selector(cols, store.n_vars(), gather_cols)});
        const bool dense = mode == "dense" || mode == "dense-prepared";
        require(dense || mode == "sparse", "invalid mode");
        ReadRequirements req;
        PreparedRead plan;
        observer.start(); const auto start = Clock::now();
        if (mode != "dense") { plan = picked.prepare_read(); req = plan.requirements(); }
        const auto prepared = Clock::now(); const auto prep_diag = observer.stop_phase();

        require(mode == "dense" || req.shape == std::vector<std::size_t>{rows.size(),cols.size()},
                "incorrect selection shape");
        require(mode == "dense" || req.dtype == DType::float32, "unexpected value type");
        require(mode == "dense" || dense == (req.layout == Layout::dense), "unexpected layout");
        std::vector<float> values(dense ? rows.size()*cols.size() : std::max<std::size_t>(req.nnz, 1));
        const std::size_t nbv = (dense ? values.size() : req.nnz)*sizeof(float);
        const std::size_t nbi = dense ? 0 : req.nnz*dtype_size(req.index_dtype);
        const std::size_t n_offset = dense ? 0 : (req.layout == Layout::csr ? rows.size() : cols.size()) + 1;
        const std::size_t nbo = dense ? 0 : n_offset*dtype_size(req.offset_dtype);
        std::vector<std::uint64_t> index(std::max<std::size_t>((nbi+7)/8,1)), offset((nbo+7)/8);
        auto dst = DenseBuffer::from(values.data(), {rows.size(), cols.size()});
        SparseBuffer sparse{values.data(), index.data(), offset.data(), req.dtype,
                            req.index_dtype, req.offset_dtype, req.nnz, req.nnz, n_offset};
        observer.start(); const auto begin = Clock::now();
        if (!dense) plan.read_into(sparse);
        else if (mode == "dense-prepared") plan.read_into(dst);
        else picked.read_into(dst);
        const auto end = Clock::now(); const auto transfer_diag = observer.stop_phase();

        auto digest = kFnvOffset;
        auto bytes = [&](const void* p, std::size_t n) {
            auto b = static_cast<const unsigned char*>(p);
            for (std::size_t i = 0; i < n; ++i) hash_byte(digest, b[i]);
        };
        bytes(values.data(), nbv); bytes(index.data(), nbi); bytes(offset.data(), nbo);
        require(hex64(digest) == expected, "caller-buffer output differs from independent oracle");
        std::cout << "{\"case\":\"" << name << "\",\"golden\":\"pass\",\"nnz\":" << req.nnz
                  << ",\"payload_bytes\":" << (nbv+nbi+nbo) << ",\"output_fnv1a64\":\"" << expected << "\""
                  << ",\"preparation_ns\":" << ns(start,prepared) << ",\"transfer_ns\":" << ns(begin,end)
                  << ",\"operation_ns\":" << ns(start,prepared)+ns(begin,end);
        if (observer.enabled) {
            std::cout << ",\"prep_diag\":"; print_observation(prep_diag);
            std::cout << ",\"transfer_diag\":"; print_observation(transfer_diag);
        }
        std::cout << "}\n";
    } catch (const std::exception& e) {
        std::cerr << "hdf5_read_reuse: " << e.what() << '\n';
        return 1;
    }
}
