// Keep the independent CSR oracle, native-width checks, FNV canonicalization,
// and optional counter observer identical to the qualified collection probe.
#include "reference_cpp_probe.cpp"

int main(int argc, char** argv) {
    try {
        require(argc == 2 || (argc == 3 && std::string_view(argv[2]) == "--diagnostic"),
                "usage: small-collection-cpp FIXTURE_PATH [--diagnostic]");
        const bool diagnostic = argc == 3;

        // Match Python's selectors and keep their construction outside both
        // open and output-operation clocks.
        std::vector<std::ptrdiff_t> row_positions, col_positions;
        row_positions.reserve(64); col_positions.reserve(64);
        for (std::ptrdiff_t i = 0; i < 256; i += 4) {
            row_positions.push_back(i);
            col_positions.push_back(i);
        }
        auto row_selection = indices(std::move(row_positions));
        auto col_selection = indices(std::move(col_positions));
        DatasetOptions options{};
        options.check_axis_names = false;
        options.backed.validation = Validation::full;

        Observer observer(diagnostic);
        observer.start();
        const auto start = Clock::now();
        auto owner = open_h5ads(argv[1], options);
        const auto opened = Clock::now();
        const auto open_observation = observer.stop_phase();

        observer.start();
        const auto selection_start = Clock::now();
        auto view = owner.slice(std::move(row_selection), std::move(col_selection));
        const auto selected = Clock::now();
        const auto selection_observation = observer.stop_phase();

        observer.start();
        const auto preparation_start = Clock::now();
        auto prepared = view.prepare_x();
        const auto req = prepared.requirements();
        const auto preparation_done = Clock::now();
        const auto preparation_observation = observer.stop_phase();

        observer.start();
        const auto allocation_start = Clock::now();
        Buffers buffers(req);
        const auto allocated = Clock::now();
        const auto allocation_observation = observer.stop_phase();

        observer.start();
        const auto transfer_start = Clock::now();
        prepared.read_into(buffers.dst);
        const auto finished = Clock::now();
        const auto transfer_observation = observer.stop_phase();

        verify_small(buffers, req);
        Observation total = open_observation;
        add(total, selection_observation);
        add(total, preparation_observation);
        add(total, allocation_observation);
        add(total, transfer_observation);

        std::cout << "{\"case\":\"dataset-small\",\"api\":\"open_h5ads_slice_prepare_x_read_into\""
                  << ",\"fixture\":\"" << argv[1] << "\",\"validation\":\"full\",\"check_axis_names\":false"
                  << ",\"source_shape\":[256,256],\"shape\":[64,64],\"selector\":\"obs=0:256:4;var=0:256:4\""
                  << ",\"nnz\":" << req.nnz
                  << ",\"value_dtype\":\"" << dtype_name(req.dtype) << "\",\"value_width_bytes\":" << dtype_size(req.dtype)
                  << ",\"index_dtype\":\"" << dtype_name(req.index_dtype) << "\",\"index_width_bytes\":" << dtype_size(req.index_dtype)
                  << ",\"offset_dtype\":\"" << dtype_name(req.offset_dtype) << "\",\"offset_width_bytes\":" << dtype_size(req.offset_dtype)
                  << ",\"golden\":\"pass\",\"output_fnv1a64\":\"" << fnv_csr(buffers, req) << '"'
                  << ",\"open_ns\":" << ns(start, opened)
                  << ",\"slice_ns\":" << ns(selection_start, selected)
                  << ",\"prepare_requirements_ns\":" << ns(preparation_start, preparation_done)
                  << ",\"output_allocation_ns\":" << ns(allocation_start, allocated)
                  << ",\"read_into_ns\":" << ns(transfer_start, finished)
                  << ",\"end_to_end_ns\":" << ns(start, finished);
        if (observer.enabled) {
            print_diagnostic(total, open_observation, selection_observation,
                             preparation_observation, allocation_observation,
                             transfer_observation, true);
        }
        std::cout << "}\n";
    } catch (const std::exception& e) {
        std::cerr << "small-collection-cpp: " << e.what() << '\n';
        return 1;
    }
}
