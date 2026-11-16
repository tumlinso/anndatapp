#!/usr/bin/env python3
"""Create the isolated multichunk raster fixture; destination must not exist."""
import argparse
import json
import os
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).parent.parent))
from python_family import _install_zarr_wakeup_if_requested, file_inventory


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('destination', type=Path)
    args = parser.parse_args()
    path = args.destination.absolute()
    if path.exists():
        raise FileExistsError(f'refusing existing destination: {path}')
    _install_zarr_wakeup_if_requested()
    import dask.array as da
    import numpy as np
    import spatialdata as sd
    from spatialdata.models import Image2DModel
    y = np.arange(512, dtype=np.uint32)[:, None]
    x = np.arange(512, dtype=np.uint32)[None, :]
    pixels = ((512 * y + x) % 65536).astype(np.uint16)[None, :, :]
    image = Image2DModel.parse(da.from_array(pixels, chunks=(1, 128, 128)),
                               dims=('c', 'y', 'x'), c_coords=['DAPI'], scale_factors=[], chunks=(1, 128, 128))
    sd.SpatialData(images={'image': image}).write(path, overwrite=False,
                                                     raster_compressor={'zstd': 3})
    metadata = json.loads((path / 'images/image/s0/zarr.json').read_text())
    if (metadata['shape'] != [1, 512, 512] or
        metadata['chunk_grid']['configuration']['chunk_shape'] != [1, 128, 128] or
        not any(codec['name'] == 'zstd' for codec in metadata['codecs'])):
        raise RuntimeError('written raster metadata differs from required shape/chunks/codec')
    files = file_inventory(path)
    size = sum(item['bytes'] for item in files.values())
    if size > 1024 * 1024:
        raise RuntimeError(f'fixture {size} bytes exceeds 1 MiB; preserved for inspection')
    print(json.dumps({'fixture': str(path), 'bytes': size, 'files': files,
                      'shape': [1, 512, 512], 'chunks': [1, 128, 128],
                      'codec': 'zstd', 'wakeup': os.environ.get('ANNDATAPP_INTEROP_ZARR_WAKEUP', '0')}, sort_keys=True))

if __name__ == '__main__':
    main()
