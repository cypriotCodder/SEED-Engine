#!/usr/bin/env python3
"""Makes Seed Editor releases that installed editors can find and verify.

  release_editor.py keygen KEY_DIR
      Makes the release signing key, KEY_DIR/update-key.pem, and prints the public key to build
      editors with (-DSEED_UPDATE_PUBLIC_KEY=...). Keep the .pem private and backed up: losing it
      means installed editors can no longer be updated.

  release_editor.py package APP OUT_DIR --key KEY.pem [--notes FILE] [--sign-identity ID]
      Packs Seed Editor.app (from the seed_editor_app build target) into OUT_DIR as
      Seed-Editor-<version>-macos.zip and writes the signed feed OUT_DIR/update.json beside it.
      --sign-identity re-signs the app with a Developer ID first (hardened runtime); the default
      keeps the ad hoc signature, which runs only on Macs that built or allowed it.

Upload both files to one release of the public releases repository; see docs/releasing.md.
The feed format is documented in editor/update.hpp. Needs macOS (ditto, codesign) and openssl.
"""
import argparse
import hashlib
import json
import os
import plistlib
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

PRODUCT = "seed-editor"


def run(*command, capture=False):
    result = subprocess.run(command, check=False, capture_output=True)
    if result.returncode != 0:
        sys.exit(f"{command[0]} failed: {result.stderr.decode(errors='replace').strip()}")
    return result.stdout if capture else None


def public_key_hex(key):
    """The uncompressed P-256 public key: the last 65 bytes of its DER SubjectPublicKeyInfo."""
    der = run("openssl", "ec", "-in", str(key), "-pubout", "-outform", "DER", capture=True)
    if len(der) != 91 or der[-65] != 4:
        sys.exit(f"{key} is not an uncompressed P-256 key")
    return der[-65:].hex()


def make_feed(zip_path, version, notes, key):
    """The signed feed text for a release zip; the editor checks it with the public key."""
    data = Path(zip_path).read_bytes()
    manifest = json.dumps({
        "format": 1,
        "product": PRODUCT,
        "version": version,
        "notes": notes,
        "file": Path(zip_path).name,
        "size": len(data),
        "sha256": hashlib.sha256(data).hexdigest(),
    }, indent=2, ensure_ascii=False)
    with tempfile.TemporaryDirectory() as scratch:
        message = Path(scratch) / "manifest.json"
        message.write_text(manifest, encoding="utf-8")
        signature = run("openssl", "dgst", "-sha256", "-sign", str(key), str(message), capture=True)
    return json.dumps({"manifest": manifest, "signature": signature.hex()}, indent=2, ensure_ascii=False) + "\n"


def keygen(arguments):
    folder = Path(arguments.key_dir)
    key = folder / "update-key.pem"
    if key.exists():
        sys.exit(f"{key} already exists; a new key would orphan editors built with the old one")
    folder.mkdir(parents=True, exist_ok=True)
    old_mask = os.umask(0o077)
    try:
        run("openssl", "ecparam", "-name", "prime256v1", "-genkey", "-noout", "-out", str(key))
    finally:
        os.umask(old_mask)
    print(f"Private key: {key} (keep it secret and backed up)")
    print(f"Public key, for -DSEED_UPDATE_PUBLIC_KEY=\n{public_key_hex(key)}")


def package(arguments):
    app = Path(arguments.app)
    info = plistlib.loads((app / "Contents" / "Info.plist").read_bytes())
    if info.get("CFBundleIdentifier") != "games.seed.editor":
        sys.exit(f"{app} is not Seed Editor.app")
    version = info["CFBundleShortVersionString"]
    notes = Path(arguments.notes).read_text(encoding="utf-8") if arguments.notes else ""
    out = Path(arguments.out_dir)
    out.mkdir(parents=True, exist_ok=True)
    zip_path = out / f"Seed-Editor-{version}-macos.zip"
    with tempfile.TemporaryDirectory() as scratch:
        staged = Path(scratch) / app.name
        shutil.copytree(app, staged, symlinks=True)
        if arguments.sign_identity:
            for target in (staged / "Contents" / "MacOS" / "seed_player", staged):
                run("codesign", "--force", "--options", "runtime", "--timestamp",
                    "--sign", arguments.sign_identity, str(target))
        run("codesign", "--verify", "--deep", "--strict", str(staged))
        zip_path.unlink(missing_ok=True)
        run("ditto", "-c", "-k", "--keepParent", str(staged), str(zip_path))
    feed = out / "update.json"
    feed.write_text(make_feed(zip_path, version, notes, arguments.key), encoding="utf-8")
    print(f"Wrote {zip_path.name} and {feed.name} to {out} for Seed Editor {version}.")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    making = commands.add_parser("keygen", help="make the release signing key")
    making.add_argument("key_dir")
    making.set_defaults(run=keygen)
    packing = commands.add_parser("package", help="zip Seed Editor.app and sign its feed")
    packing.add_argument("app")
    packing.add_argument("out_dir")
    packing.add_argument("--key", required=True, help="the private key from keygen")
    packing.add_argument("--notes", help="a UTF-8 text file of release notes shown before installing")
    packing.add_argument("--sign-identity", help="a Developer ID Application identity to sign with")
    packing.set_defaults(run=package)
    arguments = parser.parse_args()
    arguments.run(arguments)


if __name__ == "__main__":
    main()
