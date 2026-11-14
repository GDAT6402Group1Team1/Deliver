"""下载并校验固定版本的本地识别依赖，仅写入 Saved/DeliverySpeech"""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import shutil
import tarfile
import urllib.request


PROJECT = pathlib.Path(__file__).resolve().parents[2]
ROOT = PROJECT / "Saved" / "DeliverySpeech"
MANIFEST = pathlib.Path(__file__).with_name("runtime_manifest.json")


def digest(path: pathlib.Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def owned(path: pathlib.Path) -> pathlib.Path:
    result = path.resolve()
    if not result.is_relative_to(ROOT.resolve()):
        raise ValueError(f"Output escaped Saved/DeliverySpeech: {result}")
    return result


def matches(path: pathlib.Path, entry: dict) -> bool:
    return path.is_file() and path.stat().st_size == entry["size"] and digest(path) == entry["sha256"]


def download(entry: dict, destination: pathlib.Path, verify_only: bool) -> None:
    destination = owned(destination)
    if matches(destination, entry):
        return
    if verify_only:
        raise ValueError(f"Missing or mismatched pinned file: {destination}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    partial = owned(destination.with_name(destination.name + ".part"))
    print(f"Downloading {entry['name']} ({entry['size']} bytes)", flush=True)
    request = urllib.request.Request(entry["url"], headers={"User-Agent": "DeliverySpeech/1"})
    with urllib.request.urlopen(request, timeout=90) as source, partial.open("wb") as target:
        shutil.copyfileobj(source, target, length=1024 * 1024)
    if not matches(partial, entry):
        raise ValueError(f"Download failed SHA256/size verification: {entry['name']}")
    partial.replace(destination)


def prepare(*, verify_only: bool = False, fixtures: bool = False) -> dict:
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    if manifest["schema_version"] != 1 or manifest["abi_version"] != 1:
        raise ValueError("Unsupported speech manifest version")
    package = manifest["runtime"]
    archive_path = ROOT / "downloads" / package["name"]
    download(package, archive_path, verify_only)
    sdk = owned(ROOT / "sdk" / package["directory"])
    inventory = owned(ROOT / "sdk" / "files.sha256.json")
    if not inventory.is_file():
        if verify_only:
            raise ValueError("SDK inventory is missing; run preparation first")
        sdk.parent.mkdir(parents=True, exist_ok=True)
        with tarfile.open(archive_path) as archive:
            for member in archive.getmembers():
                if not (member.isfile() or member.isdir()):
                    raise ValueError("SDK archive contains an unexpected link or special file")
                output = owned(sdk.parent / member.name)
                if not output.is_relative_to(sdk):
                    raise ValueError("SDK archive contains an unexpected top-level directory")
            archive.extractall(sdk.parent, filter="data")
        hashes = {str(path.relative_to(sdk)).replace("\\", "/"): digest(path)
                  for path in sorted(sdk.rglob("*")) if path.is_file()}
        inventory.write_text(json.dumps({"archive_sha256": package["sha256"], "files": hashes}, indent=2) + "\n", encoding="utf-8")
    recorded = json.loads(inventory.read_text(encoding="utf-8"))
    if recorded["archive_sha256"] != package["sha256"]:
        raise ValueError("SDK inventory belongs to a different package")
    for relative, expected in recorded["files"].items():
        path = owned(sdk / relative)
        if not path.is_relative_to(sdk) or not path.is_file() or digest(path) != expected:
            raise ValueError(f"Extracted SDK file changed: {relative}")
    runtime = owned(ROOT / "runtime")
    for entry in manifest["model"]["files"]:
        download(entry, runtime / "models" / entry["name"], verify_only)
    for entry in manifest["licenses"]:
        download(entry, runtime / "licenses" / entry["name"], verify_only)
    if fixtures:
        for entry in manifest["fixtures"]:
            download(entry, ROOT / "fixtures" / entry["name"], verify_only)
    result = {"schema_version": 1, "abi_version": 1, "sdk": str(sdk), "runtime": str(runtime),
              "model": manifest["model"], "sdk_archive_sha256": package["sha256"],
              "manifest_sha256": digest(MANIFEST)}
    if not verify_only:
        runtime.mkdir(parents=True, exist_ok=True)
        (runtime / "prepared.json").write_text(json.dumps(result, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
        shutil.copyfile(MANIFEST, runtime / "runtime_manifest.json")
    print(json.dumps({"verified": True, "runtime": str(runtime), "sdk": str(sdk)}, ensure_ascii=False), flush=True)
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--verify-only", action="store_true", help="校验现有依赖，不下载、不修改")
    parser.add_argument("--fixtures", action="store_true", help="包含开发回归样本，样本不会进入 runtime 打包目录")
    args = parser.parse_args()
    prepare(verify_only=args.verify_only, fixtures=args.fixtures)
