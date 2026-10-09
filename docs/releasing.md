# Releasing the editor and updates

Installed copies of `Seed Editor.app` update themselves from a signed feed. This page covers the
feed format, the one-time setup, and the steps for each release. Only the editor updates itself.
Exported games have no network code.

## How an update works

1. At most once a day (or from **Help → Check for Updates…**), the editor fetches the feed URL
   `SEED_UPDATE_FEED`. By default this is
   `https://github.com/cypriotCodder/seed-releases/releases/latest/download/update.json`, the
   newest release in the public `seed-releases` repository. The source repository stays private.
2. The feed's signature is checked against `SEED_UPDATE_PUBLIC_KEY`, which is built into the
   editor. If the signature is wrong, or the manifest has any unknown, missing or out-of-range
   field, the editor rejects the feed.
3. If the manifest's version is newer than the editor's version (and not skipped), the editor
   downloads the zip from the same folder as the feed into
   `~/Library/Application Support/Seed/Seed Editor/updates/`. It keeps the zip only if its size
   and SHA-256 match the manifest.
4. When the user chooses **Restart to Update**, or otherwise when they quit, the editor installs the
   update:
   - It checks the zip's hash again.
   - It unpacks the zip beside the installed app as `.Seed Editor.update/`.
   - It requires exactly `Seed Editor.app`, with bundle identifier `games.seed.editor`, the
     manifest's version, and a valid code signature.
   - It swaps the new app in by renaming folders. If anything fails, the installed app stays
     where it was. A failure is shown on the next start, and the download is offered again.

Updates are off in builds without a public key, in editors run from a build folder rather than
from `Seed Editor.app`, and in automated (`--smoke`, `--ui-test`) runs.

## Feed format

`update.json`:

```json
{"manifest": "<manifest JSON, as text>", "signature": "<hex DER ECDSA P-256 / SHA-256 signature>"}
```

The signature covers the exact UTF-8 bytes of the `manifest` string, which holds:

| Field | Meaning |
| --- | --- |
| `format` | `1` |
| `product` | `"seed-editor"` |
| `version` | `MAJOR.MINOR.PATCH`, each 0–999999, no leading zeros. Compared numerically |
| `notes` | Plain-text release notes shown before installing, at most 16 KB |
| `file` | The zip's name, in the same folder as the feed: letters, digits, `.`, `_`, `-`, ending `.zip` |
| `size` | The zip's size in bytes, 1 byte to 1 GiB |
| `sha256` | The zip's SHA-256, 64 lowercase hex digits |

A new field or meaning needs a new `format`. Editors reject formats they do not know.

## One-time setup

1. Make the release key. Store it outside the repository and back it up, because losing it means
   installed editors can never be updated again:

   ```sh
   python3 tools/release_editor.py keygen ~/SeedReleaseKey
   ```

   The command prints the public key (130 hex digits).
2. Build editors with that public key. Either set it as the default value of
   `SEED_UPDATE_PUBLIC_KEY` in `editor/CMakeLists.txt` (a public key is safe to commit), or pass
   `-DSEED_UPDATE_PUBLIC_KEY=<hex>` when configuring.
3. Create the public repository `cypriotCodder/seed-releases`. It only holds releases, so it can
   be empty apart from a README. If the feed is hosted somewhere else, set `SEED_UPDATE_FEED`; the
   zip must sit in the same folder as the feed.

## Each release

1. Raise `VERSION` in the `project()` line of the top-level `CMakeLists.txt`. The splash screen,
   About box, bundle and update check all use it.
2. Build and test the release preset. Its `seed_editor_app` target makes
   `build/release/editor/Seed Editor.app`.
3. Package the app and sign the feed:

   ```sh
   python3 tools/release_editor.py package "build/release/editor/Seed Editor.app" build/release-out \
       --key ~/SeedReleaseKey/update-key.pem --notes notes.txt \
       [--sign-identity "Developer ID Application: …"]
   ```

   Without `--sign-identity` the app keeps its ad hoc signature, which only runs on the Mac that
   built it. To give it to other Macs, sign it with a Developer ID and notarize the zip
   (`xcrun notarytool submit … --wait`) before publishing. The tool does not notarize.
4. Publish both files in one release (not a draft or pre-release, which `latest` skips):

   ```sh
   gh release create v<version> build/release-out/Seed-Editor-<version>-macos.zip \
       build/release-out/update.json -R cypriotCodder/seed-releases --notes-file notes.txt
   ```

The first install still has to be done by hand: download the zip and move `Seed Editor.app` to
`/Applications` or anywhere else the user can write to. After that, the app updates itself in place.

## Tests

`editor_updates` tests version parsing, manifest and feed validation, real signature checks,
downloads, refusal of tampered downloads, skipping, and the checks made before installing. Its
fixtures in `editor/tests/update` are signed with a **test-only** key.

The `editor_update*` tests need Python 3 and openssl. They package an app marked 99.0.0, sign
its feed with the test key, and have a copy of the built `Seed Editor.app` download, verify and
install it.
