"""Independent MuData readers/writers: pass the native test binary and fixture directory."""
import asyncio
import os
import shutil
from pathlib import Path
import subprocess
import tempfile
import sys
import warnings

if os.environ.get("ANNDATAPP_INTEROP_ZARR_WAKEUP") == "1":
    class PollPolicy(asyncio.DefaultEventLoopPolicy):
        def new_event_loop(self):
            loop = super().new_event_loop()
            def tick():
                loop.call_later(0.01, tick)
            loop.call_later(0.01, tick)
            return loop
    asyncio.set_event_loop_policy(PollPolicy())

import anndata as ad
import h5py
import mudata as md
import numpy as np
import pandas as pd
import zarr

warnings.simplefilter("ignore", FutureWarning)


def fixture():
    rna = ad.AnnData(X=np.array([[1, 2], [3, 4]], dtype=np.float32),
                     obs=pd.DataFrame(index=["a", "c"]), var=pd.DataFrame(index=["g1", "g2"]))
    rna.raw = rna.copy()
    protein = ad.AnnData(X=np.array([[5, 6], [7, 8]], dtype=np.float32),
                         obs=pd.DataFrame(index=["b", "c"]), var=pd.DataFrame(index=["p1", "p2"]))
    data = md.MuData({"rna": rna, "protein": protein})
    data.obs["quality"] = np.array([10, 20, 30], dtype=np.int32)
    data.obsm["embedding"] = np.arange(6, dtype=np.float64).reshape(3, 2)
    data.varm["embedding"] = np.arange(8, dtype=np.float64).reshape(4, 2)
    data.obsp["connectivity"] = np.eye(3, dtype=np.float32)
    data.varp["connectivity"] = np.eye(4, dtype=np.float32)
    data.uns["project"] = "fixture"
    return data


def check(data):
    assert list(data.mod) == ["rna", "protein"]
    assert data.obs_names.tolist() == ["a", "c", "b"]
    assert data.var_names.tolist() == ["g1", "g2", "p1", "p2"]
    assert data.obs["quality"].tolist() == [10, 20, 30]
    assert data.obsmap["rna"].reshape(-1).tolist() == [1, 2, 0]
    assert data.obsmap["protein"].reshape(-1).tolist() == [0, 2, 1]
    assert data.varmap["protein"].reshape(-1).tolist() == [0, 0, 1, 2]
    assert data.mod["rna"].raw is not None
    np.testing.assert_array_equal(data.mod["rna"].X, [[1, 2], [3, 4]])
    np.testing.assert_array_equal(data.obsm["embedding"], np.arange(6).reshape(3, 2))
    np.testing.assert_array_equal(data.varm["embedding"], np.arange(8).reshape(4, 2))
    np.testing.assert_array_equal(data.obsp["connectivity"], np.eye(3))
    assert data.uns["project"] == "fixture"


def main():
    binary, directory = sys.argv[1:3]
    root = Path(directory)
    root.mkdir(parents=True, exist_ok=True)
    legacy = os.environ.get("ANNDATAPP_MUDATA_LEGACY") == "1"
    mode = sys.argv[3] if len(sys.argv) > 3 else "run"
    if mode == "run":
        root = Path(tempfile.mkdtemp(prefix="roundtrip-", dir=root))
    data = fixture() if mode != "check" else None
    for fmt, suffix in (("hdf5", "h5mu"), ("zarr", "zarr")):
        source = root / ("reference." + suffix)
        result = root / ("native." + suffix)
        if mode != "check" and fmt == "hdf5":
            data.write_h5mu(source)
            if legacy:
                with h5py.File(source, "r+") as f:
                    del f.attrs["axis"]
        elif mode != "check":
            data.write_zarr(str(source))
            if legacy:
                del zarr.open_group(str(source), mode="r+").attrs["axis"]
        if mode == "generate":
            continue
        if mode != "check":
            subprocess.run([binary, "--io", fmt, str(source), str(result)], check=True)
        loaded = md.read_h5mu(result) if fmt == "hdf5" else md.read_zarr(str(result))
        check(loaded)
        if fmt == "hdf5":
            with open(result, "rb") as f:
                assert f.read(6) == b"MuData"
        if mode == "run":
            for defect in ("subtype", "axis", "unknown"):
                bad = root / (defect + "." + suffix)
                if fmt == "hdf5":
                    shutil.copyfile(source, bad)
                    with h5py.File(bad, "r+") as f:
                        if defect == "subtype":
                            f["mod/rna"].attrs["encoding-subtype"] = "treedata"
                        elif defect == "axis":
                            f.attrs["axis"] = 7
                        else:
                            f.create_group("future-extension")
                else:
                    shutil.copytree(source, bad)
                    f = zarr.open_group(str(bad), mode="r+")
                    if defect == "subtype":
                        f["mod/rna"].attrs["encoding-subtype"] = "treedata"
                    elif defect == "axis":
                        f.attrs["axis"] = 7
                    else:
                        f.create_group("future-extension")
                subprocess.run([binary, "--reject", fmt, str(bad), defect], check=True)
            for axis in (1, -1):
                axis_data = md.MuData({k: v.copy() for k, v in data.mod.items()}, axis=axis)
                axis_input = root / (f"axis-{axis}-reference." + suffix)
                axis_output = root / (f"axis-{axis}-native." + suffix)
                if fmt == "hdf5":
                    axis_data.write_h5mu(axis_input)
                else:
                    axis_data.write_zarr(str(axis_input))
                subprocess.run([binary, "--axis-io", fmt, str(axis_input), str(axis_output), str(axis)], check=True)
                result_data = md.read_h5mu(axis_output) if fmt == "hdf5" else md.read_zarr(str(axis_output))
                assert result_data.axis == axis
                assert result_data.obs_names.tolist() == axis_data.obs_names.tolist()
                assert result_data.var_names.tolist() == axis_data.var_names.tolist()
                for name in axis_data.mod:
                    np.testing.assert_array_equal(result_data.obsmap[name], axis_data.obsmap[name])
                    np.testing.assert_array_equal(result_data.varmap[name], axis_data.varmap[name])
                    np.testing.assert_array_equal(result_data.mod[name].X, axis_data.mod[name].X)
    print("MuData independent fixtures generated" if mode == "generate" else "MuData independent HDF5/Zarr round trips passed")


if __name__ == "__main__":
    main()
