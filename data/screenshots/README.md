# AppStream screenshot

`collection.png` captures the actual Pirwa window with three stopped synthetic
libvirt test-driver definitions. It contains no real guest image or guest screen.
Its source definitions are in `examples/screenshot-machines.xml`.

From the repository root, regenerate it on an isolated 1280×800 GNOME desktop:

```sh
meson compile -C build test-ui
./scripts/test-gnome.py --output build/qa/catalog-screenshot-new -- \
  ./build/tests/test-ui --uri "test://$PWD/examples/screenshot-machines.xml" \
  --width 1088 --duration 1 --snapshot "$PWD/data/screenshots/collection.png"
```

The QA tool's ordinary settling period lets selected resources finish loading.
The PNG is app-only, at scale 1, 1088×680 (16:10), with the default light theme.
Inspect the image after capture. This is not a startup timing run.

The metadata points at the matching path on the repository's default branch.
The repository is private and the asset is not published yet; public URL/image
reachability remains a release gate. An offline schema check does not close it.
