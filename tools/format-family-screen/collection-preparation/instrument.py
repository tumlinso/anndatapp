#!/usr/bin/env python3
"""Copy a source tree and add matched, diagnostic-only collection phase hooks."""
import argparse
from pathlib import Path
import shutil

HERE = Path(__file__).resolve().parent


def replace_once(path, old, new):
    text = path.read_text()
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{path}: expected one match, found {count}: {old[:100]!r}")
    path.write_text(text.replace(old, new, 1))


def add_phase_include(path):
    replace_once(path, "module;\n", 'module;\n#include "phase_probe.h"\n')


def instrument(source):
    for name in ("backed_array.cpp", "backed_hdf5.cpp", "dataset.cpp"):
        add_phase_include(source / "src" / name)

    replace_once(source / "CMakeLists.txt",
        "src/backed_records.cpp src/backed_geometry.cpp src/streaming.cpp",
        "src/phase_probe.cpp src/backed_records.cpp src/backed_geometry.cpp src/streaming.cpp")
    with (source / "CMakeLists.txt").open("a") as cmake:
        cmake.write(r'''

# Added only to the copied source tree by collection-preparation/instrument.py.
enable_language(C)
add_executable(collection-phase-probe tools/format-family-screen/collection-preparation/driver.cpp)
target_link_libraries(collection-phase-probe PRIVATE anndatapp ${CMAKE_DL_LIBS})
add_library(collection-io SHARED tools/format-family-screen/collection-preparation/io.c)
target_link_libraries(collection-io PRIVATE ${CMAKE_DL_LIBS})
add_library(collection-alloc SHARED tools/format-family-screen/collection-preparation/alloc.cpp)
add_library(collection-metadata SHARED tools/format-family-screen/collection-preparation/metadata.c)
target_link_libraries(collection-metadata PRIVATE ${CMAKE_DL_LIBS})
''')
    shutil.copyfile(HERE / "phase_probe.h", source / "src" / "phase_probe.h")
    shutil.copyfile(HERE / "phase_probe.cpp", source / "src" / "phase_probe.cpp")

    dataset = source / "src" / "dataset.cpp"
    replace_once(dataset,
        "PreparedAnnDataSetRead AnnDataSetView::prepare_x() const {\n",
        "PreparedAnnDataSetRead AnnDataSetView::prepare_x() const {\n    Phase phase_prepare(0);\n")
    replace_once(dataset,
        "    check_selection_indexes(*impl_, obs_, var_);\n",
        "    { Phase phase_validation(1); check_selection_indexes(*impl_, obs_, var_); }\n")
    replace_once(dataset,
        "    if (!count) return;\n    {\n        // Variable comparisons retain only component IDs;",
        "    if (!count) return;\n    {\n        Phase phase_var(2);\n        // Variable comparisons retain only component IDs;")
    replace_once(dataset,
        "    // gathers consume the full collection selector budget.\n    for_segments",
        "    // gathers consume the full collection selector budget.\n    Phase phase_obs(3);\n    for_segments")
    replace_once(dataset,
        "    auto first = impl_->files.front().x();\n    auto& out = p->requirements;\n    out.layout = first.layout(); out.dtype = first.dtype(); out.shape = {n_obs(), n_vars()};\n    if (out.layout != Layout::dense) { out.index_dtype = first.index_dtype(); out.offset_dtype = first.offset_dtype(); }",
        "    auto& out = p->requirements;\n    phase_enter(4);\n    auto first = impl_->files.front().x();\n    out.layout = first.layout(); out.dtype = first.dtype(); out.shape = {n_obs(), n_vars()};\n    if (out.layout != Layout::dense) { out.index_dtype = first.index_dtype(); out.offset_dtype = first.offset_dtype(); }\n    phase_leave(4);")
    replace_once(dataset,
        "        auto read = selected_x(*impl_, obs_, var_, s).prepare_read();",
        "        Phase phase_component(6); auto read = selected_x(*impl_, obs_, var_, s).prepare_read();")
    replace_once(dataset,
        "        if (out.layout != Layout::csc) retain_piece(*p, s, SIZE_MAX, std::move(read), charged);",
        "        if (out.layout != Layout::csc) { Phase phase_retain(9); retain_piece(*p, s, SIZE_MAX, std::move(read), charged); }")
    replace_once(dataset,
        "    return PreparedAnnDataSetRead(std::move(p));\n}",
        "    phase_piece(p->pieces.size(), p->retained);\n    return PreparedAnnDataSetRead(std::move(p));\n}")

    array = source / "src" / "backed_array.cpp"
    replace_once(array,
        "        r.nnz = scan_sparse(n, d, nullptr, require_original_widths, scratch_limit);",
        "        { Phase phase_scan(8); r.nnz = scan_sparse(n, d, nullptr, require_original_widths, scratch_limit); }")
    replace_once(array,
        "    auto d = describe(node_);\n    auto p = std::make_shared<detail::PreparedReadImpl>();",
        "    auto d = [&] { Phase phase_descriptor(7); return describe(node_); }();\n    auto p = std::make_shared<detail::PreparedReadImpl>();")

    hdf = source / "src" / "backed_hdf5.cpp"
    replace_once(hdf,
        "    void read(const std::vector<std::size_t>& start, const std::vector<std::size_t>& count, void* out) override {\n        const auto n = select(start, count);",
        "    void read(const std::vector<std::size_t>& start, const std::vector<std::size_t>& count, void* out) override {\n        Phase phase_numeric(10);\n        const auto n = select(start, count);")
    replace_once(hdf,
        '        c_.check(H5Dread(dataset_, memory, memory_space_, file_space_, H5P_DEFAULT, out), path_, "read numerical rectangle");',
        '        { Phase phase_h5dread(11); c_.check(H5Dread(dataset_, memory, memory_space_, file_space_, H5P_DEFAULT, out), path_, "read numerical rectangle"); }')
    replace_once(hdf,
        "    std::unique_ptr<NumericTransfer> prepare_numeric(std::string_view path) override {\n        ensure_open();",
        "    std::unique_ptr<NumericTransfer> prepare_numeric(std::string_view path) override {\n        Phase phase_numeric(10);\n        ensure_open();")
    replace_once(hdf,
        "    void read_numeric(std::string_view path, const std::vector<std::size_t>& start,\n                      const std::vector<std::size_t>& count, void* destination) override {\n        ensure_open();",
        "    void read_numeric(std::string_view path, const std::vector<std::size_t>& start,\n                      const std::vector<std::size_t>& count, void* destination) override {\n        Phase phase_numeric(10);\n        ensure_open();")
    replace_once(hdf,
        '        c_.check(H5Dread(dataset, memory_type, r.memory_space, r.file_space, H5P_DEFAULT, destination), path, "read numerical rectangle");',
        '        { Phase phase_h5dread(11); c_.check(H5Dread(dataset, memory_type, r.memory_space, r.file_space, H5P_DEFAULT, destination), path, "read numerical rectangle"); }')
def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("source", type=Path, help="unmodified checkout or source snapshot")
    p.add_argument("destination", type=Path, help="new directory for instrumented source copy")
    a = p.parse_args()
    src = a.source.resolve(strict=True)
    dst = a.destination.resolve()
    if not src.is_dir():
        p.error(f"source is not a directory: {src}")
    if src == dst:
        p.error("destination must differ from the source tree")
    if src in dst.parents and dst.relative_to(src).parts[0] != "build":
        p.error("an in-tree destination must be under the ignored build/ directory")
    if dst.exists():
        p.error(f"destination exists; preserve it and choose a new directory: {dst}")
    dst.parent.mkdir(parents=True, exist_ok=True)
    source_copy = dst / "source"
    shutil.copytree(src, source_copy, symlinks=True,
                    ignore=shutil.ignore_patterns(".git", "build", ".venv", "__pycache__", ".todo-orchestrator"))
    probe_copy = source_copy / "tools/format-family-screen/collection-preparation"
    probe_copy.mkdir(parents=True, exist_ok=True)
    for item in HERE.iterdir():
        if item.is_file() and item.name not in {"__pycache__"}:
            shutil.copyfile(item, probe_copy / item.name)
    instrument(source_copy)
    print(source_copy)
    print(f"Configure with: cmake -S {source_copy} -B {dst / 'build'} -G Ninja \\")
    print("  -DCMAKE_CXX_COMPILER=clang++-18 -DCMAKE_BUILD_TYPE=Release \\")
    print("  -DANNDATAPP_ENABLE_ZARR=OFF -DANNDATAPP_ENABLE_PARQUET=OFF \\")
    print("  -DANNDATAPP_BUILD_TESTS=OFF -DANNDATAPP_BUILD_EXAMPLES=OFF -DANNDATAPP_BUILD_BENCHMARKS=OFF")
    print(f"Build phase probe: cmake --build {dst / 'build'} --parallel 2 --target collection-phase-probe collection-io collection-alloc collection-metadata")


if __name__ == "__main__":
    main()
