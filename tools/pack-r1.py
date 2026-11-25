#!/usr/bin/env python3
"""Overlay a built player and R1 resources on a verified official Sonix R1 image."""

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RESOURCE = Path("usr/resource/sonix")
CHUNK = 524288
OTA_REPO = "pbjorklund/sonix-audiobooks"


def r1_build_info(data, build_id):
    info = {**data, "build_version": build_id, "ota-repo": OTA_REPO}
    # The internet updater compares against OS_version, not the build label.
    if re.fullmatch(r"[0-9]+(?:\.[0-9]+){2,3}", build_id):
        info["OS_version"] = build_id
    return info


def digest(data):
    return hashlib.md5(data).hexdigest()  # Required by the device's OTA format.


def run(*args, capture=False):
    return subprocess.run(
        args, check=True, stdout=subprocess.PIPE if capture else subprocess.DEVNULL
    ).stdout


def read_image(ota, name):
    tables = list(ota.glob(f"ota_md5_{name}.*"))
    if len(tables) != 1:
        raise ValueError(f"Expected one checksum table for {name}")
    table = tables[0]
    expected = previous = table.suffix[1:]
    checks = table.read_text().splitlines()
    parts = sorted(ota.glob(f"{name}.[0-9]*"))
    if not parts or len(parts) != len(checks):
        raise ValueError(f"Wrong chunk count for {name}")
    image = bytearray()
    for index, (part, checksum) in enumerate(zip(parts, checks)):
        piece = part.read_bytes()
        if part.name != f"{name}.{index:04d}.{previous}" or digest(piece) != checksum:
            raise ValueError(f"Invalid chunk chain: {part.name}")
        image.extend(piece)
        previous = checksum
    metadata = (ota / "ota_update.in").read_text()
    stanza = f"img_name={name}\nimg_size={len(image)}\nimg_md5={expected}"
    if digest(image) != expected or stanza not in metadata:
        raise ValueError(f"Image or OTA metadata mismatch for {name}")
    return image


def replace_rootfs(ota, image):
    name = "rootfs.squashfs"
    (old,) = ota.glob(f"ota_md5_{name}.*")
    metadata = ota / "ota_update.in"
    new = previous = digest(image)
    pattern = rf"(img_name=rootfs\.squashfs\n)img_size=\d+\nimg_md5={re.escape(old.suffix[1:])}"
    text, count = re.subn(
        pattern, rf"\g<1>img_size={len(image)}\nimg_md5={new}", metadata.read_text()
    )
    if count != 1:
        raise ValueError("Expected one rootfs stanza in OTA metadata")
    for part in ota.glob(f"{name}.[0-9]*"):
        part.unlink()
    old.unlink()
    checks = []
    for index, offset in enumerate(range(0, len(image), CHUNK)):
        piece = image[offset : offset + CHUNK]
        (ota / f"{name}.{index:04d}.{previous}").write_bytes(piece)
        previous = digest(piece)
        checks.append(previous)
    (ota / f"ota_md5_{name}.{new}").write_text("\n".join(checks) + "\n")
    metadata.write_text(text)


def pack(args):
    base = args.base.resolve(strict=True)
    if (
        not re.fullmatch(r"[0-9a-f]{64}", args.base_sha256)
        or hashlib.sha256(base.read_bytes()).hexdigest() != args.base_sha256
    ):
        raise ValueError("Official base SHA-256 mismatch")
    output = args.output.resolve()
    if output.exists():
        raise ValueError(f"Output already exists: {output}")
    player = ROOT / "sonix-player/sonix_player"
    resources = ROOT / "sonix-packer/assets/R1" / RESOURCE
    if not player.is_file() or (player.parent / "streaming-keys.key").exists():
        raise ValueError("Build the target player without a local streaming seal key")
    abi = run("make", "-C", str(player.parent), "check-abi", capture=True)
    if b"check-abi: no symbol beyond glibc 2.22." not in abi:
        raise ValueError("The target ABI check did not run successfully")
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=output.parent) as work:
        work = Path(work)
        iso, tree, readback = (work / name for name in ("iso", "tree", "readback"))
        run(args.seven_zip, "x", str(base), f"-o{iso}", "-y")
        ota = iso / "ota_v0"
        kernel = read_image(ota, "xImage")
        rootfs = work / "rootfs"
        rootfs.write_bytes(read_image(ota, "rootfs.squashfs"))
        run("unsquashfs", "-processors", "4", "-d", str(tree), str(rootfs))
        info = tree / RESOURCE / "components/system-info.json"
        data = json.loads(info.read_text())
        if (
            data.get("device-name") != "HiBy R1"
            or not (tree / "usr/bin/bluealsa").is_file()
        ):
            raise ValueError(
                "Base must be an official Sonix image for the original R1 with BlueALSA"
            )
        forbidden = {
            "streaming-keys.ini",
            "streaming-keys.key",
            "streaming-keys.bin",
            ".env",
        }
        if any(p.name in forbidden for p in tree.rglob("*")):
            raise ValueError(
                "Base contains credential files; use an unprovisioned image"
            )
        if (tree / "usr/bin/sonix_player").is_symlink() or info.is_symlink():
            raise ValueError("Unexpected symlink at an overlay destination")
        shutil.copyfile(player, tree / "usr/bin/sonix_player")
        (tree / "usr/bin/sonix_player").chmod(0o755)
        assets = sorted((resources / "language").glob("*.ini"))
        if len(assets) != 12:
            raise ValueError("Expected 12 R1 language files")
        for src in assets:
            shutil.copyfile(src, tree / RESOURCE / "language" / src.name)
        packaged_info = (json.dumps(r1_build_info(data, args.build_id), indent=4) + "\n").encode()
        info.write_bytes(packaged_info)
        rootfs.unlink()
        run(
            "mksquashfs",
            str(tree),
            str(rootfs),
            "-processors",
            "4",
            "-comp",
            "lzo",
            "-all-root",
            "-noappend",
            "-no-progress",
            "-exit-on-error",
        )
        replace_rootfs(ota, rootfs.read_bytes())
        artifact = work / "r1.upt"
        run("xorrisofs", "-o", str(artifact), "-J", "-r", str(iso))
        run(args.seven_zip, "x", str(artifact), f"-o{readback}", "-y")
        if read_image(readback / "ota_v0", "xImage") != kernel:
            raise ValueError("Packaged kernel changed")
        rootfs.write_bytes(read_image(readback / "ota_v0", "rootfs.squashfs"))
        for path, src in [
            ("usr/bin/sonix_player", player),
            *[(str(RESOURCE / "language" / p.name), p) for p in assets],
        ]:
            if (
                run("unsquashfs", "-cat", str(rootfs), path, capture=True)
                != src.read_bytes()
            ):
                raise ValueError(f"Packaged file differs: {path}")
        if run("unsquashfs", "-cat", str(rootfs), str(RESOURCE / "components/system-info.json"), capture=True) != packaged_info:
            raise ValueError("Packaged OTA channel or build metadata differs")
        result = work / "verified"
        result.mkdir()
        artifact.rename(result / "r1.upt")
        shutil.copyfile(player, result / "sonix_player")
        sums = "".join(
            f"{hashlib.sha256((result / name).read_bytes()).hexdigest()}  {name}\n"
            for name in ("r1.upt", "sonix_player")
        )
        (result / "SHA256SUMS").write_text(sums)
        result.rename(output)
    print(
        f"Verified both OTA chains, player and {len(assets)} language files in {output}\n{sums}",
        end="",
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", type=Path, required=True)
    parser.add_argument("--base-sha256", required=True)
    parser.add_argument("--build-id", required=True)
    parser.add_argument(
        "--output", type=Path, required=True, help="New output directory"
    )
    parser.add_argument("--seven-zip", default="7z")
    os.umask(0o077)
    args = parser.parse_args()
    if not re.fullmatch(r"[A-Za-z0-9._-]{1,40}", args.build_id):
        parser.error(
            "Use 1–40 letters, digits, dots, underscores or hyphens for --build-id"
        )
    try:
        pack(args)
    except (OSError, ValueError, subprocess.CalledProcessError) as exc:
        parser.exit(1, f"pack-r1: {exc}\n")


if __name__ == "__main__":
    main()
