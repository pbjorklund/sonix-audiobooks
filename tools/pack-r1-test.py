#!/usr/bin/env python3
import importlib.util
import tempfile
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location(
    "pack_r1", Path(__file__).with_name("pack-r1.py")
)
assert spec is not None and spec.loader is not None
packer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(packer)


class OtaTests(unittest.TestCase):
    def test_round_trip_and_corrupt_chunk(self):
        with tempfile.TemporaryDirectory() as work:
            ota = Path(work)
            name = "rootfs.squashfs"
            old = packer.digest(b"old")
            (ota / f"ota_md5_{name}.{old}").write_text(old + "\n")
            (ota / f"{name}.0000.{old}").write_bytes(b"old")
            (ota / "ota_update.in").write_text(
                f"img_name={name}\nimg_size=3\nimg_md5={old}\n"
            )
            image = b"A" * packer.CHUNK + b"last chunk"
            packer.replace_rootfs(ota, image)
            self.assertEqual(packer.read_image(ota, name), image)
            self.assertFalse((ota / f"{name}.0000.{old}").exists())
            part = next(ota.glob(f"{name}.0001.*"))
            part.write_bytes(b"bad")
            with self.assertRaisesRegex(ValueError, "Invalid chunk chain"):
                packer.read_image(ota, name)


if __name__ == "__main__":
    unittest.main()
