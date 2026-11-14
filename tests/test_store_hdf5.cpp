#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

import anndatapp.h5ad;

using namespace anndatapp;

namespace {
void require(bool ok, std::string_view message) {
    if (!ok) throw std::runtime_error(std::string(message));
}
void run(const std::filesystem::path& path) {
    auto store = create_hdf5_store(path.string(), {}, 512);
    auto attrs = Element::mapping();
    attrs.set("max", Element(std::numeric_limits<std::uint64_t>::max()));
    auto floats = Array::empty<float>({2, 2});
    auto fp = floats.edit().dense<float>().data();
    fp[0] = 1.25f; fp[1] = -2.5f; fp[2] = 3; fp[3] = 4;
    attrs.set("float", Element(std::move(floats)));
    auto flags = Array::empty<bool>({2, 2});
    auto bp = flags.edit().dense<bool>().data();
    bp[0] = true; bp[1] = false; bp[2] = false; bp[3] = true;
    attrs.set("bool", Element(std::move(flags)));
    attrs.set("mod-order", Element(StringArray::from_strings({0}, {})));
    store.set_attributes("/", attrs.view());
    auto update = Element::mapping(); update.set("new", Element(std::int64_t{7}));
    store.set_attributes("/", update.view());
    auto bad = Element::mapping(); bad.set("new", Element(std::int64_t{9}));
    bad.set("unsupported", Element::mapping());
    bool rejected = false;
    try { store.set_attributes("/", bad.view()); }
    catch (const Error& e) { rejected = e.code() == ErrorCode::unsupported_encoding; }
    require(rejected, "nested attribute must be rejected");
    store.close();
    std::ifstream input(path, std::ios::binary); input.seekg(512);
    char signature[8]{}; input.read(signature, 8);
    require(std::string_view(signature, 8) == std::string_view("\x89HDF\r\n\x1a\n", 8), "HDF5 signature must follow user block");
    auto read = open_hdf5_store(path.string());
    auto result = read.attributes(); auto view = result.view();
    require(view.at("max").dtype() == DType::uint64 && view.at("max").unsigned_integer() == std::numeric_limits<std::uint64_t>::max(), "uint64 scalar");
    require(view.at("new").signed_integer() == 7, "preflight and merge must preserve earlier attributes");
    auto f = view.at("float").array(); auto fdata = f.dense<float>();
    require(f.dtype() == DType::float32 && f.rank() == 2 && f.extent(0) == 2 && f.extent(1) == 2 && fdata.data()[1] == -2.5f, "float array");
    auto b = view.at("bool").array(); auto bdata = b.dense<bool>();
    require(b.dtype() == DType::boolean && b.rank() == 2 && bdata.data()[0] && !bdata.data()[1] && bdata.data()[3], "boolean array");
    auto order = view.at("mod-order").string_array();
    require(order.rank() == 1 && order.size() == 0, "empty string array");
    read.close();
    auto limited = create_hdf5_store(path.string() + ".budget", {}, 0, 64);
    (void)limited.create_strings("/values", {9});
    std::vector<std::string> values(9, "value");
    rejected = false;
    try { limited.write_strings("/values", {0}, {9}, values); }
    catch (const Error& e) { rejected = e.code() == ErrorCode::invalid_argument; }
    require(rejected, "string pointer staging must fail when its session budget is insufficient");
    rejected = false;
    try { limited.write_string("/too-large", std::string(64, 'x')); }
    catch (const Error& e) { rejected = e.code() == ErrorCode::invalid_argument; }
    require(rejected && !limited.exists("/too-large"), "scalar transfer capacity must fail before target creation");
    limited.close();
    auto unchanged = open_hdf5_store(path.string() + ".budget");
    require(unchanged.strings("/values").string(0).empty(), "rejected string pointer staging changed payload");
    unchanged.close();
}
}
int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("expected temporary HDF5 path");
        run(argv[1]);
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
