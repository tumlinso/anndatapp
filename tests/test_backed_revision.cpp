#include <array>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

import anndatapp;
using namespace anndatapp;

void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
template<class F> void rejects(F f) {
    try { f(); } catch (const Error&) { return; }
    throw std::runtime_error("expected backed operation to fail");
}
int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("fixture path required");
        std::filesystem::create_directories(argv[1]);
        const auto path = (std::filesystem::path(argv[1]) / "revision.zarr").string();
        auto data = AnnData::empty(2, 2);
        auto x = Array::empty<float>({2, 2});
        auto values = x.edit().dense<float>();
        for (int i = 0; i < 4; ++i) values.data()[i] = float(i);
        data.set_x(std::move(x));
        auto sparse = Array::csr<float, std::int32_t, std::int32_t>(2, 2, 2);
        auto sv = sparse.edit().csr<float, std::int32_t, std::int32_t>();
        sv.values()[0] = 1; sv.values()[1] = 2;
        sv.unsafe_indices()[0] = 0; sv.unsafe_indices()[1] = 1;
        sv.unsafe_offsets()[0] = 0; sv.unsafe_offsets()[1] = 1; sv.unsafe_offsets()[2] = 2;
        data.set_layer("sparse", std::move(sparse));
        write_zarr(path, data);
        auto store = open_zarr(path, BackedOptions{.mode = AccessMode::read_write});
        auto whole = store.x();
        auto selected = whole.select({indices{1}, all});
        auto before = whole.content_revision();
        auto patch = Array::empty<float>({1, 2});
        patch.edit().dense<float>().data()[0] = 20;
        patch.edit().dense<float>().data()[1] = 21;
        selected.write(patch.view());
        require(whole.content_revision() > before && selected.content_revision() == whole.content_revision(), "selected write revision");
        require(whole.read().view().dense<float>().data()[2] == 20, "retained descriptor after write");
        auto transfer = whole.prepare_transfer();
        std::array<float, 2> update{30, 31};
        before = whole.content_revision();
        transfer.write_from({0, 0}, {1, 2}, DenseBuffer::from(update.data(), {1, 2}));
        require(whole.content_revision() > before && whole.read().view().dense<float>().data()[0] == 30, "transfer write revision");
        auto prepared = whole.prepare_read();
        std::array<float, 4> dst{99, 99, 99, 99};
        prepared.preflight(DenseBuffer::from(dst.data(), {2, 2}));
        require(dst == std::array<float, 4>{99, 99, 99, 99}, "preflight wrote destination");
        rejects([&] { prepared.preflight(DenseBuffer::from(dst.data(), {1, 4})); });
        require(dst == std::array<float, 4>{99, 99, 99, 99}, "failed preflight wrote destination");
        auto sp = store.layer("sparse");
        auto sp_read = sp.prepare_read();
        std::array<float, 2> sp_values{99, 99};
        std::array<std::int32_t, 2> sp_indices{99, 99};
        std::array<std::int32_t, 3> sp_offsets{99, 99, 99};
        SparseBuffer sp_buffer{sp_values.data(), sp_indices.data(), sp_offsets.data(), DType::float32,
            DType::int32, DType::int32, 2, 2, 3};
        sp_read.preflight(sp_buffer);
        auto bad = sp_buffer; bad.offsets_capacity = 2;
        rejects([&] { sp_read.preflight(bad); });
        require(sp_values[0] == 99 && sp_indices[0] == 99 && sp_offsets[0] == 99, "sparse preflight wrote destination");
        auto sparse_patch = sp.read();
        sparse_patch.edit().csr<float, std::int32_t, std::int32_t>().values()[0] = 50;
        before = sp.content_revision();
        sp.write(sparse_patch.view());
        require(sp.content_revision() > before && sp.read().view().csr<float, std::int32_t, std::int32_t>().values()[0] == 50, "sparse write revision");
        store.close();
        rejects([&] { (void)whole.content_revision(); });
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
