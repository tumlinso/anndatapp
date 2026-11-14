import anndata;
import anndatapp;

int main() {
    auto a = anndatapp::AnnData::empty(2, 3);
    return a.n_obs() == 2 && a.n_vars() == 3 ? 0 : 1;
}
