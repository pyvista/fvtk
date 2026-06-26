"""Regression: the IOImage module is present in the built wheel.

IOImage provides the core image readers and writers (PNG/JPEG/TIFF/BMP) that
screenshots and texture loading depend on. The module is enabled explicitly in
the minimal module closure rather than relying on it being pulled in
transitively through another module's dependency chain, so that image IO cannot
silently disappear if that chain ever changes.

This test asserts that the module imports and that a representative reader and
writer class resolve from it. It exercises only the wrapper/import path, so it
needs no GL context and runs anywhere the wheel installs.
"""


def test_io_image_classes_import():
    from cvista.vtkIOImage import vtkPNGReader, vtkPNGWriter

    # Instantiation confirms the wrapped class is real, not just a name.
    assert vtkPNGReader() is not None
    assert vtkPNGWriter() is not None
