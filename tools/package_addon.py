#!/usr/bin/env python3
"""Package an EWatch addon for upload to EWatch Cloud.

Usage:
    python3 tools/package_addon.py addons/<Folder> [--no-build]

Reads <addon>/addon.json, builds the PlatformIO env `ewatch` (skip with
--no-build), and writes <addon>/dist/:

    <id>-<version>/bootloader.bin    flash parts, one file per region
    <id>-<version>/partitions.bin
    <id>-<version>/boot_app0.bin
    <id>-<version>/firmware.bin
    <id>-<version>-merged.bin        single image for offset 0x0 (wipes NVS)
    manifest.json                    ESP Web Tools manifest (parts; keeps NVS)
    addon.json                       metadata + offsets, sizes, sha256
    icon.svg
    web/                             companion page, if the addon has web/index.html
    <id>-<version>.zip               all of the above, ready to upload

The parts manifest skips the NVS region, so installing or switching addons
keeps the watch's saved settings (theme, WiFi networks, haptics, ...). The
merged image fills the gaps with 0xFF and therefore erases them; use it for
single-file flashers or a deliberate clean install.
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import zipfile
from datetime import datetime, timezone
from pathlib import Path

HOME = Path.home()
PIO = Path(os.environ.get("PIO", HOME / ".platformio/penv/bin/pio"))
PIO_PY = HOME / ".platformio/penv/bin/python"
ESPTOOL = HOME / ".platformio/packages/tool-esptoolpy/esptool.py"
BOOT_APP0 = (HOME / ".platformio/packages/framework-arduinoespressif32"
             "/tools/partitions/boot_app0.bin")
ENV = "ewatch"

TYPES = {"watchface", "app", "game", "tool"}
CAPABILITIES = {
    "imu", "haptic", "touch", "ble", "wifi", "rtc-wake",
    "background-steps", "background-ble", "serial", "storage",
}
REQUIRED = ["id", "name", "version", "type", "summary", "description",
            "author", "license", "capabilities", "icon"]


def fail(msg):
    sys.exit(f"package_addon: {msg}")


def validate(meta, addon_dir):
    missing = [k for k in REQUIRED if k not in meta]
    if missing:
        fail(f"addon.json is missing {missing}")
    if not re.fullmatch(r"[a-z0-9][a-z0-9-]{1,30}", meta["id"]):
        fail("id must be kebab-case, 2-31 chars")
    if not re.fullmatch(r"\d+\.\d+\.\d+(-[0-9A-Za-z.-]+)?", meta["version"]):
        fail("version must be semver, e.g. 1.0.0")
    if meta["type"] not in TYPES:
        fail(f"type must be one of {sorted(TYPES)}")
    if len(meta["summary"]) > 120:
        fail("summary must be at most 120 characters")
    unknown = set(meta["capabilities"]) - CAPABILITIES
    if unknown:
        fail(f"unknown capabilities {sorted(unknown)}; "
             f"allowed: {sorted(CAPABILITIES)}")
    if not (addon_dir / meta["icon"]).is_file():
        fail(f"icon {meta['icon']} not found")


def parse_partitions(path):
    """Return {(type, subtype): (offset, size, label)} from a partitions.bin."""
    table = {}
    data = path.read_bytes()
    for i in range(0, len(data) - 31, 32):
        magic, ptype, subtype, offset, size, label, _flags = struct.unpack(
            "<HBBII16sI", data[i:i + 32])
        if magic != 0x50AA:
            break
        table[(ptype, subtype)] = (offset, size, label.rstrip(b"\0").decode())
    return table


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("addon_dir", type=Path)
    ap.add_argument("--no-build", action="store_true",
                    help="package the existing .pio build without rebuilding")
    args = ap.parse_args()

    addon = args.addon_dir.resolve()
    meta_path = addon / "addon.json"
    if not meta_path.is_file():
        fail(f"{meta_path} not found")
    meta = json.loads(meta_path.read_text())
    validate(meta, addon)

    if not args.no_build:
        print(f"building {meta['id']} ...", flush=True)
        r = subprocess.run([str(PIO), "run", "-e", ENV, "-d", str(addon)])
        if r.returncode:
            fail("PlatformIO build failed")

    build = addon / ".pio" / "build" / ENV
    parts_src = {
        "bootloader.bin": build / "bootloader.bin",
        "partitions.bin": build / "partitions.bin",
        "boot_app0.bin": BOOT_APP0,
        "firmware.bin": build / "firmware.bin",
    }
    for name, p in parts_src.items():
        if not p.is_file():
            fail(f"missing {name} at {p} (build first)")

    table = parse_partitions(parts_src["partitions.bin"])
    app = table.get((0x00, 0x10)) or table.get((0x00, 0x00))
    otadata = table.get((0x01, 0x00))
    nvs = table.get((0x01, 0x02))
    if not app or not otadata:
        fail("partition table needs an app (ota_0/factory) and otadata entry")
    fw_size = parts_src["firmware.bin"].stat().st_size
    if fw_size > app[1]:
        fail(f"firmware.bin is {fw_size} bytes; app partition is {app[1]}")

    offsets = {
        "bootloader.bin": 0x0,
        "partitions.bin": 0x8000,
        "boot_app0.bin": otadata[0],
        "firmware.bin": app[0],
    }

    stem = f"{meta['id']}-{meta['version']}"
    dist = addon / "dist"
    if dist.exists():
        shutil.rmtree(dist)
    parts_dir = dist / stem
    parts_dir.mkdir(parents=True)

    files = []
    for name, src in parts_src.items():
        dst = parts_dir / name
        shutil.copyfile(src, dst)
        files.append({
            "path": f"{stem}/{name}",
            "offset": offsets[name],
            "offset_hex": hex(offsets[name]),
            "size": dst.stat().st_size,
            "sha256": sha256(dst),
        })

    merged = dist / f"{stem}-merged.bin"
    cmd = [str(PIO_PY), str(ESPTOOL), "--chip", "esp32s3", "merge_bin",
           "-o", str(merged), "--flash_mode", "keep", "--flash_freq", "keep",
           "--flash_size", "keep"]
    for f in files:
        cmd += [f["offset_hex"], str(dist / f["path"])]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        fail(f"esptool merge_bin failed:\n{r.stdout}\n{r.stderr}")

    manifest = {
        "name": meta["name"],
        "version": meta["version"],
        "new_install_prompt_erase": False,
        "builds": [{
            "chipFamily": "ESP32-S3",
            "parts": [{"path": f["path"], "offset": f["offset"]}
                      for f in files],
        }],
    }
    (dist / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")

    shutil.copyfile(addon / meta["icon"], dist / Path(meta["icon"]).name)

    # Companion web pages (e.g. a Web Bluetooth configurator) ship alongside
    # the firmware so EWatch Cloud can host them over HTTPS.
    web_src = addon / "web"
    companion = None
    if (web_src / "index.html").is_file():
        shutil.copytree(web_src, dist / "web")
        companion = "web/index.html"

    out_meta = dict(meta)
    out_meta["icon"] = Path(meta["icon"]).name
    out_meta["build"] = {
        "built_at": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "chip": "ESP32-S3",
        "flash_size": "4MB",
        "app_size": fw_size,
        "app_partition_size": app[1],
        "app_partition_used_pct": round(100 * fw_size / app[1], 1),
        "nvs_offset": hex(nvs[0]) if nvs else None,
        "parts": files,
        "merged": {
            "path": merged.name,
            "offset": 0,
            "size": merged.stat().st_size,
            "sha256": sha256(merged),
            "note": "fills gaps with 0xFF, so flashing it erases NVS settings",
        },
        "web_flasher_manifest": "manifest.json",
        "companion_page": companion,
    }
    (dist / "addon.json").write_text(json.dumps(out_meta, indent=2) + "\n")

    zip_path = dist / f"{stem}.zip"
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as z:
        for p in sorted(dist.rglob("*")):
            if p.is_file() and p != zip_path:
                z.write(p, p.relative_to(dist))
        readme = addon / "README.md"
        if readme.is_file():
            z.write(readme, "README.md")

    print(f"\n{meta['name']} {meta['version']}: app {fw_size} bytes "
          f"({out_meta['build']['app_partition_used_pct']}% of the app partition)")
    for p in sorted(dist.rglob("*")):
        if p.is_file():
            print(f"  {p.relative_to(addon)}  ({p.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
