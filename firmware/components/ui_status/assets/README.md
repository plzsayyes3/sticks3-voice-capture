# Rady status sprites

`rady_sprite_sheet_source.png` is the supplied five-color/eight-pose source
sheet. Regenerate the firmware icons with:

```sh
python3 scripts/prepare-rady-icons.py
```

The generated `.bin` files are transparent 112×112 BGRA byte arrays for LVGL
ARGB8888. They are embedded by `../CMakeLists.txt`.

Current device mapping:

- Standby and supporting UI scenes: orange Rady.
- Recording (SD card or internal flash): green Rady with music notes.
- Pink SD-recording is not mapped yet: `recording_store_on_sd()` exposes the
  state, but another icon (~50KB) does not fit the 2MiB OTA slot until the
  partition layout grows.
- Sky-blue Wi-Fi-connected is not mapped: `ui_status_set_syncing()` is shown
  both before connecting and for failed syncs, so it is not a connected state.
