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
- Recording to internal flash: green Rady with music notes.
- Recording to the SD card: pink Rady with music notes (chosen at boot from
  `recording_store_on_sd()`).
- Sky-blue Wi-Fi-connected is not mapped: `ui_status_set_syncing()` is shown
  both before connecting and for failed syncs, so it is not a connected state.
