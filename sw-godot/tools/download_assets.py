# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Alexandre Aparecido Rodrigues Peres
"""Download and verify the locked Poly Haven CC0 texture sets for the test map."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import urllib.error
import urllib.request
from pathlib import Path
from typing import Any

API = "https://api.polyhaven.com"
MAPS = {"Diffuse": ("jpg",), "nor_dx": ("jpg",), "Rough": ("jpg",), "Displacement": ("png",)}


def write_keep_import(target: Path, asset_id: str) -> None:
    """Keep auxiliary texture maps in the project as source payloads."""
    if target.name.startswith(asset_id + "_diff_"):
        return
    target.with_name(target.name + ".import").write_text(
        '[remap]\n\nimporter="keep"\n\n[deps]\n\n'
        f'source_file="res://assets/regions/{asset_id}/{target.name}"\n\n'
        '[params]\n\nmetadata/compress/mode="0"\n',
        encoding="utf-8",
    )


def fetch_json(url: str) -> Any:  # noqa: ANN401
    # URLs come from the fixed HTTPS API base or its HTTPS file metadata.
    request = urllib.request.Request(  # noqa: S310
        url, headers={"User-Agent": "SwarmstreamNetlab/1.0"}
    )  # noqa: S310
    with urllib.request.urlopen(request, timeout=30) as response:  # noqa: S310
        return json.load(response)


def download(url: str, target: Path, expected_md5: str) -> None:
    target.parent.mkdir(parents=True, exist_ok=True)
    digest = hashlib.md5(usedforsecurity=False)
    request = urllib.request.Request(  # noqa: S310
        url, headers={"User-Agent": "SwarmstreamNetlab/1.0"}
    )  # noqa: S310
    temporary = target.with_suffix(target.suffix + ".tmp")
    try:
        with (
            urllib.request.urlopen(request, timeout=120) as response,  # noqa: S310
            temporary.open("wb") as output,
        ):
            while chunk := response.read(1024 * 1024):
                digest.update(chunk)
                output.write(chunk)
        if digest.hexdigest() != expected_md5:
            raise ValueError(f"MD5 mismatch for {target.name}")
        temporary.replace(target)
    except (OSError, urllib.error.URLError, ValueError):
        temporary.unlink(missing_ok=True)
        raise


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--resolution", choices=("4k", "8k", "16k"), default="8k")
    parser.add_argument(
        "--output", type=Path, default=Path(__file__).parents[1] / "project/assets/regions"
    )
    parser.add_argument(
        "--only",
        help="ids separados por vírgula (ex.: rock_path); baixa só esses conjuntos do lock",
    )
    args = parser.parse_args()
    output = args.output.resolve()
    rows: list[dict[str, str | int]] = []
    lock_path = Path(__file__).parents[1] / "assets.lock.json"
    lock = json.loads(lock_path.read_text(encoding="utf-8-sig"))
    locked_assets = lock["assets"]
    if args.only:
        wanted = [item.strip() for item in args.only.split(",") if item.strip()]
        known = {asset["id"] for asset in locked_assets}
        unknown = sorted(set(wanted) - known)
        if unknown:
            raise SystemExit(f"--only: ids fora do assets.lock.json: {', '.join(unknown)}")
        locked_assets = [asset for asset in locked_assets if asset["id"] in set(wanted)]
    metadata_catalog = fetch_json(f"{API}/assets?type=textures")
    for locked_asset in locked_assets:
        asset_id = locked_asset["id"]
        region = locked_asset["region"]
        metadata = metadata_catalog.get(asset_id)
        if metadata is None:
            raise ValueError(f"Poly Haven asset not found: {asset_id}")
        if metadata["files_hash"] != locked_asset["files_hash"]:
            raise ValueError(f"Asset changed since assets.lock.json was created: {asset_id}")
        files = fetch_json(f"{API}/files/{asset_id}")
        for map_name, extensions in MAPS.items():
            for extension in extensions:
                map_entry = files.get(map_name)
                resolution_entry = (
                    map_entry.get(args.resolution, {}) if isinstance(map_entry, dict) else {}
                )
                file_data = (
                    resolution_entry.get(extension) if isinstance(resolution_entry, dict) else None
                )
                if file_data is None:
                    continue
                relative_path = Path(asset_id) / Path(file_data["url"]).name
                target = output / relative_path
                if (
                    not target.is_file()
                    or hashlib.md5(target.read_bytes(), usedforsecurity=False).hexdigest()
                    != file_data["md5"]
                ):
                    download(file_data["url"], target, file_data["md5"])
                write_keep_import(target, asset_id)
                rows.append(
                    {
                        "region": region,
                        "asset_id": asset_id,
                        "asset_name": locked_asset["name"],
                        "map": map_name,
                        "resolution": args.resolution,
                        "license": "CC0 1.0 Universal",
                        "license_url": "https://creativecommons.org/publicdomain/zero/1.0/",
                        "source_url": f"https://polyhaven.com/a/{asset_id}",
                        "path": relative_path.as_posix(),
                        "size_bytes": file_data["size"],
                        "md5": file_data["md5"],
                    }
                )
                print(
                    f"{region}/{asset_id}: {target.name} "
                    f"({file_data['size'] / 1_000_000:.1f} MB)",
                    flush=True,
                )
    output.parent.mkdir(parents=True, exist_ok=True)
    catalog_dir = Path(__file__).parents[1]
    # Com --only o inventário é parcial: não sobrescreve o assets.csv completo do repositório.
    csv_path = output.parent / "assets-only.csv" if args.only else catalog_dir / "assets.csv"
    with csv_path.open("w", newline="", encoding="utf-8-sig") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    print(
        f"Catalogued {len(rows)} files from {len(locked_assets)} CC0 assets at {args.resolution}."
    )
    print("Powered by Poly Haven (https://polyhaven.com).")


if __name__ == "__main__":
    main()
