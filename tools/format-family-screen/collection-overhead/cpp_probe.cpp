// Reuse the qualified consumer's buffers, canonical hashes and scoped counters.
#define main collection_reference_main
#include "../collection-fix/cpp_probe.cpp"
#undef main
#include <fstream>
#include <numeric>

namespace {
struct Case {
    std::string name, manifest;
    std::size_t source_rows, source_cols, source_nnz, repeats;
    std::vector<std::ptrdiff_t> rows, cols;
};
Case load_case(const char* path) {
    std::ifstream in(path);
    require(bool(in), "cannot open case specification");
    Case c; std::size_t nr, nc;
    std::getline(in, c.name); std::getline(in, c.manifest);
    in >> c.source_rows >> c.source_cols >> c.source_nnz >> nr >> nc >> c.repeats;
    require(c.source_nnz == 16 || c.source_nnz == 128, "unknown fixture formula");
    require(nr && nc && c.repeats && c.repeats <= 5, "invalid case dimensions");
    c.rows.resize(nr); c.cols.resize(nc);
    for (auto& i : c.rows) { in >> i; require(i >= 0 && std::size_t(i) < c.source_rows, "invalid row"); }
    for (auto& i : c.cols) { in >> i; require(i >= 0 && std::size_t(i) < c.source_cols, "invalid column"); }
    require(bool(in), "truncated case specification");
    return c;
}
struct NativeSelector { Selection value; const char* kind; };
NativeSelector native_selector(const std::vector<std::ptrdiff_t>& positions, std::size_t extent) {
    const auto step = positions.size() > 1 ? positions[1] - positions[0] : 1;
    bool regular = step > 0;
    for (std::size_t i = 1; i < positions.size(); ++i)
        if (positions[i] - positions[i-1] != step) regular = false;
    if (regular && positions.front() == 0 && step == 1 && positions.size() == extent)
        return {Selection(all), "all"};
    if (regular) return {Selection(range{positions.front(), positions.back()+1, step}), step == 1 ? "contiguous_range" : "strided_range"};
    return {Selection(indices(positions)), "indices"};
}
std::string verify_and_hash(const Case& c, const Buffers& b, const ReadRequirements& req) {
    require(req.layout == Layout::csr && req.dtype == DType::float32, "expected native float32 CSR");
    require(req.shape == std::vector<std::size_t>{c.rows.size(), c.cols.size()}, "incorrect output shape");
    const std::size_t step = c.source_nnz == 16 ? 127 : 15;
    std::vector<std::pair<std::uint32_t, float>> expected;
    std::vector<std::uint32_t> columns;
    std::vector<float> values;
    std::vector<std::uint64_t> offsets{0};
    std::size_t total = 0;
    for (std::size_t i = 0; i < c.rows.size(); ++i) {
        const auto r = std::size_t(c.rows[i]);
        expected.clear();
        for (std::size_t j = 0; j < c.cols.size(); ++j) {
            const auto col = std::size_t(c.cols[j]), shift = r % 97;
            if (col < shift || (col - shift) % step) continue;
            const auto k = (col - shift) / step;
            if (k < c.source_nnz) expected.emplace_back(std::uint32_t(j), float(1 + (r+k) % 31));
        }
        const auto first = integer(b.offsets, req.offset_dtype, i);
        const auto end = integer(b.offsets, req.offset_dtype, i+1);
        require(first == total && end >= first && end <= req.nnz, "invalid sparse offsets");
        require(end-first == expected.size(), "incorrect per-row sparse count");
        std::vector<std::pair<std::uint32_t, float>> actual;
        actual.reserve(end-first);
        for (auto p = first; p < end; ++p) {
            const auto j = integer(b.indices, req.index_dtype, p);
            require(j < c.cols.size(), "invalid sparse output column");
            actual.emplace_back(std::uint32_t(j), cell<float>(b.values, p));
        }
        std::sort(actual.begin(), actual.end());
        require(actual == expected, "CSR output differs from independent formula");
        for (const auto [j,x] : actual) { columns.push_back(j); values.push_back(x); }
        total = end; offsets.push_back(end);
    }
    require(total == req.nnz, "incorrect final sparse count");
    auto hash = kFnvOffset;
    for (const auto x : values) hash_csr_value(hash, x);
    for (const auto j : columns) hash_little_endian(hash, j);
    for (const auto p : offsets) hash_little_endian(hash, p);
    return hex64(hash);
}
}

struct Phase {
    Observation existing{};
    std::uint64_t opens[2]{};
};
struct Recorder {
    Observer observer{true};
    using Snapshot = void (*)(std::uint64_t*);
    Snapshot snapshot = reinterpret_cast<Snapshot>(dlsym(RTLD_DEFAULT, "dataset_handle_snapshot"));
    std::uint64_t before[2]{};
    Recorder() { require(snapshot != nullptr, "missing dataset handle interposer"); }
    void start() { snapshot(before); observer.start(); }
    Phase stop() {
        Phase phase;
        phase.existing = observer.stop_phase();
        snapshot(phase.opens);
        for (int i = 0; i < 2; ++i) phase.opens[i] -= before[i];
        return phase;
    }
};
void print_phase(const char* name, const Phase& phase) {
    std::cout << "\"" << name << "\":{";
    std::cout << "\"dataset_opens\":" << phase.opens[0] << ",\"dataset_closes\":" << phase.opens[1] << ",\"counters\":";
    print_observation(phase.existing); std::cout << '}';
}
int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: collection-overhead-cpp CASE.txt");
        const auto c = load_case(argv[1]);
        auto rows = native_selector(c.rows, c.source_rows), cols = native_selector(c.cols, c.source_cols);
        Recorder recorder;
        recorder.start(); auto owner = open_h5ads(c.manifest); auto opened = recorder.stop();
        recorder.start(); auto view = owner.slice(std::move(rows.value), std::move(cols.value)); auto selected = recorder.stop();
        recorder.start(); auto plan = view.prepare_x(); auto req = plan.requirements(); auto prepared = recorder.stop();
        recorder.start(); Buffers buffers(req); auto allocated = recorder.stop();
        recorder.start(); plan.preflight(buffers.dst); auto preflight = recorder.stop();
        recorder.start(); plan.read_into(buffers.dst); auto first = recorder.stop();
        const auto golden = verify_and_hash(c, buffers, req);
        Phase repeats[3];
        for (auto& repeat : repeats) {
            recorder.start(); plan.read_into(buffers.dst); repeat = recorder.stop();
            require(verify_and_hash(c, buffers, req) == golden, "repeated output differs");
        }
        std::cout << "{\"case\":\"" << c.name << "\",\"components\":" << owner.n_components()
                  << ",\"shape\":[" << c.rows.size() << ',' << c.cols.size() << "],\"nnz\":" << req.nnz
                  << ",\"golden\":\"pass\",\"output_fnv1a64\":\"" << golden << "\",\"phases\":{";
        print_phase("open", opened); std::cout << ',';
        print_phase("selection", selected); std::cout << ',';
        print_phase("prepare", prepared); std::cout << ',';
        print_phase("allocate", allocated); std::cout << ',';
        print_phase("preflight", preflight); std::cout << ',';
        print_phase("first_read", first);
        for (int i = 0; i < 3; ++i) { const auto name = std::string("repeat_") + std::to_string(i+1); std::cout << ','; print_phase(name.c_str(), repeats[i]); }
        std::cout << "}}\n";
    } catch (const std::exception& e) { std::cerr << "collection-overhead-cpp: " << e.what() << '\n'; return 1; }
}
