"""Package an ESP32-S3 build for the web flasher (flasher/index.html, ESP Web Tools).

Copies the image parts listed in the build's flash_args (bootloader, partition table, OTA data, app) into
<out>/firmware/ and writes <out>/manifest.json with each part at its own offset. Parts rather than one merged image:
a merged image is contiguous from 0x0 and would overwrite the NVS partition between them (the watch's saved settings).
The page itself (flasher/*) is copied alongside.

usage: python package_flasher.py <build dir> <flasher dir> <out dir> <version>
"""
import json
import pathlib
import shutil
import sys


def main() -> None:
    build, site, out, version = (pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2]), pathlib.Path(sys.argv[3]),
                                 sys.argv[4])
    parts = []
    for line in (build / "flash_args").read_text().splitlines():
        fields = line.split()
        if len(fields) == 2 and fields[0].startswith("0x"):
            parts.append((int(fields[0], 16), build / fields[1]))
    if not parts:
        sys.exit("no image parts in %s/flash_args" % build)
    if out.exists():
        shutil.rmtree(out)
    shutil.copytree(site, out)
    fw = out / "firmware"
    fw.mkdir()
    manifest_parts = []
    for offset, path in sorted(parts):
        shutil.copy2(path, fw / path.name)
        manifest_parts.append({"path": "firmware/" + path.name, "offset": offset})
    manifest = {
        "name": "Wardrive Go Watch",
        "version": version,
        "new_install_prompt_erase": True,
        "builds": [{"chipFamily": "ESP32-S3", "parts": manifest_parts}],
    }
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    for p in manifest_parts:
        print("0x%06x  %s" % (p["offset"], p["path"]))
    print("manifest.json: version %s" % version)


if __name__ == "__main__":
    main()
