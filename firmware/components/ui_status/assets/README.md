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
- Wi-Fi joined during a sync (uploads running): sky-blue Rady with sparkles.
  `wifi_sync` reports the joined network explicitly; scanning and failed syncs
  keep the orange "Sync" screen.
