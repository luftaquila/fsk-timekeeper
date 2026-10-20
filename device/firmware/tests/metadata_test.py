"""The DFU and outer ZIP metadata must describe the exact linked image."""
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
import zipfile

spec = importlib.util.spec_from_file_location("metadata", Path(__file__).resolve().parents[1] / "tools/firmware_metadata.py")
metadata = importlib.util.module_from_spec(spec)
spec.loader.exec_module(metadata)


class MetadataTest(unittest.TestCase):
    def test_package_copies_identical_metadata_and_binds_files(self):
        with tempfile.TemporaryDirectory() as d:
            folder = Path(d)
            info = dict(schema_version=1, app_start=0x26000, app_limit=0xF2000, version="v1.2.3")
            image = b"\0" * 8 + metadata.MARKER + metadata.encoded(info).encode() + b"\0"
            binary = folder / "firmware.bin"
            binary.write_bytes(image)
            path = folder / "firmware.zip"
            with zipfile.ZipFile(path, "w") as z:
                z.writestr("firmware.bin", image)
                z.writestr("firmware.dat", b"init")
                z.writestr("manifest.json", json.dumps({"manifest": {"dfu_version": 0.5,
                    "application": {"bin_file": "firmware.bin", "dat_file": "firmware.dat"}}}))
            metadata.package(path, binary)
            with zipfile.ZipFile(path) as z:
                content = z.read(metadata.NAME)
                self.assertEqual(content, path.with_name(metadata.NAME).read_bytes())
                manifest = json.loads(content)
                self.assertEqual(manifest["version"], "v1.2.3")
                self.assertEqual(manifest["files"]["firmware.bin"]["sha256"], hashlib.sha256(image).hexdigest())
            metadata.package(path, binary)  # repeated packaging replaces metadata, never duplicates it
            with zipfile.ZipFile(path) as z:
                self.assertEqual(z.namelist().count(metadata.NAME), 1)
            binary.write_bytes(image + b"changed")
            with self.assertRaisesRegex(ValueError, "differs"):
                metadata.package(path, binary)

    def test_missing_or_duplicate_identity_is_rejected(self):
        for image in (b"old firmware", metadata.MARKER + b"{}\0" + metadata.MARKER + b"{}\0"):
            with self.assertRaisesRegex(ValueError, "exactly one"):
                metadata.embedded_info(image)


if __name__ == "__main__":
    unittest.main()
