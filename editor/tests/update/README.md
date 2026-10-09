# Update test fixtures

`test-key.pem` is a **test-only** P-256 key. It signs these fixtures and the update test in
`editor/CMakeLists.txt`, and nothing else. Real releases are signed with a private key that is
never committed (see `docs/releasing.md`). Its public key is:

```
04d1902a9ce5970d41cca0f0095758f327b47546ef3a5679efa846fe43776046d254af9a03a10ec0e16a920eb0333e1c3bfc984606b9710eb2b92d8e3cd3251142
```

- `feed.json`: version 9.0.0, signed by `make_feed` in `tools/release_editor.py` for
  `Seed-Editor-9.0.0-macos.zip`. That file is a stand-in, not a real zip: the unit tests only
  download and verify it.
- `tampered/`: the same signed feed beside a zip changed after signing, which must be refused.
