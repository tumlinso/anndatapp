#include <type_traits>

import anndata;
import anndatapp;

static_assert(std::is_nothrow_move_constructible_v<anndatapp::PreparedAnnDataSetRead>);
static_assert(!std::is_copy_constructible_v<anndatapp::PreparedAnnDataSetRead>);

int main() {
    auto a = anndatapp::AnnData::empty(2, 3);
    if (a.n_obs() != 2 || a.n_vars() != 3) return 1;
    anndatapp::PreparedAnnDataSetRead empty;
    try { (void)empty.requirements(); }
    catch (const anndatapp::Error& e) {
        return e.code() == anndatapp::ErrorCode::invalid_argument ? 0 : 2;
    }
    return 3;
}
