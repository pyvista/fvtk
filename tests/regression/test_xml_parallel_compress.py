"""Regression: vtkXMLWriter parallel block compression is byte-exact.

fvtk compresses the independent BlockSize-sized data blocks of an XML binary
write (.vti/.vtp/.vtu ...) concurrently instead of one-at-a-time, then writes the
resulting zlib streams sequentially in block order. Because each block is an
independent zlib stream and the write order is preserved, the compressed bytes
must be identical to the serial path -- only the work order changes.

The serial path is stock VTK code that fvtk leaves untouched, so proving
"parallel payload == serial payload" within fvtk also proves the parallel
compression matches stock's, without depending on a full-file comparison.

We compare only the ``AppendedData`` payload (the concatenated compression
headers + zlib block streams), NOT the whole file: VTK's XML *header* writer has
a pre-existing run-to-run quirk (an empty ``<DataArray/>`` is sometimes emitted
self-closed and sometimes with an explicit close tag), which is unrelated to
compression and which this change never touches. The compressed payload itself
is deterministic.

The writer's parallel path is gated on >1 estimated thread, so we force the
serial path by capping vtkSMPTools to one thread and compare against a default
multi-threaded write of the identical dataset.
"""

import numpy as np
import pytest

from fvtk.vtkCommonCore import vtkSMPTools
from fvtk.vtkCommonDataModel import vtkImageData
from fvtk.vtkIOXML import vtkXMLImageDataWriter, vtkXMLImageDataReader, vtkXMLPolyDataWriter
from fvtk.vtkFiltersSources import vtkSphereSource
from fvtk.util.numpy_support import numpy_to_vtk, vtk_to_numpy


def _image_with_large_array(dtype):
    # 64^3 = 262144 values; at 8 bytes that is ~2 MiB == ~64 of the 32 KiB
    # compression blocks, so the parallel path runs many blocks across threads.
    n = 64
    img = vtkImageData()
    img.SetDimensions(n, n, n)
    vals = (np.arange(n * n * n, dtype=np.float64) * 0.25).astype(dtype)
    arr = numpy_to_vtk(vals, deep=True)
    arr.SetName("scalars")
    img.GetPointData().SetScalars(arr)
    return img


def _appended_payload(raw):
    """Return the raw ``AppendedData`` bytes (compression headers + zlib blocks)."""
    marker = raw.find(b"AppendedData")
    assert marker != -1, "no AppendedData section"
    start = raw.find(b"_", marker) + 1  # appended raw data begins right after '_'
    end = raw.rfind(b"</AppendedData>")
    assert 0 < start < end
    return raw[start:end]


def _write(writer_factory, data, nthreads, tmp_path, tag):
    vtkSMPTools.Initialize(nthreads)
    effective = vtkSMPTools.GetEstimatedNumberOfThreads()
    path = str(tmp_path / f"{tag}.out")
    w = writer_factory()
    w.SetFileName(path)
    w.SetInputData(data)
    w.SetDataModeToAppended()
    w.SetCompressorTypeToZLib()
    assert w.Write() == 1
    with open(path, "rb") as fh:
        return fh.read(), effective


def _assert_parallel_equals_serial(writer_factory, data, tmp_path):
    parallel, n_par = _write(writer_factory, data, 0, tmp_path, "parallel")  # 0 == auto
    serial, n_ser = _write(writer_factory, data, 1, tmp_path, "serial")  # 1 == force serial

    # Only a genuine multi-thread-vs-single-thread comparison validates the
    # parallel path; on a single-core runner both are serial and prove nothing.
    if not (n_par > 1 and n_ser == 1):
        pytest.skip(f"cannot isolate parallel vs serial (auto={n_par}, forced={n_ser})")

    par_payload = _appended_payload(parallel)
    assert len(par_payload) > 0
    assert par_payload == _appended_payload(serial)
    return parallel


@pytest.mark.parametrize("dtype", [np.float64, np.float32])
def test_imagedata_parallel_compress_matches_serial(dtype, tmp_path):
    img = _image_with_large_array(dtype)
    raw = _assert_parallel_equals_serial(vtkXMLImageDataWriter, img, tmp_path)

    # Round-trip the parallel-written file to confirm it is also valid (not just
    # byte-identical to serial): the decoded scalars must equal the input.
    path = str(tmp_path / "roundtrip.vti")
    with open(path, "wb") as fh:
        fh.write(raw)
    reader = vtkXMLImageDataReader()
    reader.SetFileName(path)
    reader.Update()
    back = vtk_to_numpy(reader.GetOutput().GetPointData().GetScalars())
    expected = (np.arange(64 * 64 * 64, dtype=np.float64) * 0.25).astype(dtype)
    assert np.array_equal(back, expected)


def test_polydata_parallel_compress_matches_serial(tmp_path):
    # Exercises the connectivity/offset integer arrays too, not just point data.
    src = vtkSphereSource()
    src.SetThetaResolution(200)
    src.SetPhiResolution(200)
    src.Update()
    _assert_parallel_equals_serial(vtkXMLPolyDataWriter, src.GetOutput(), tmp_path)


def teardown_module(module):
    # Restore auto thread selection for any subsequent tests in the session.
    vtkSMPTools.Initialize(0)
