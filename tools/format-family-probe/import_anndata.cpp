import anndata;

int main() {
    auto data = anndatapp::AnnData::empty(16, 8);
    return data.n_obs() == 16 && data.n_vars() == 8 ? 0 : 1;
}
