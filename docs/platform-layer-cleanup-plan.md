# Platform layer cleanup

## Problem

The Windows platform layer (and its macOS twin) still makes app decisions and
draws UI of its own:

| lives in both platform layers today | why it is not platform work |
|---|---|
| native frame-timing HUD (GDI window / `DebugHudView`), each owning a `FrameStats` and formatting the same five lines | it is UI; the in-app UI already draws text and rects |
| F3 chord tracking (`f3Down`, OR-ing `ShortcutMod_F3` into key mods) | `app.cpp` already tracks F3 for the tap-alone case |
| Escape leaves fullscreen | a keybinding |
| right-drag pans, shift+right-drag / middle-drag orbits | camera policy |
| only `.wav` files may be dropped | import policy |
| File > Import shows a `.blend` filter and calls `ImportBlendFile` | import policy |
| `AppRequestRender` after Init, resize, menu command, resume, HUD toggle | the app can tell on its own |
| evaluating each command's `isEnabled` / `isChecked` for the native menu | command-table logic |

Goal: a platform layer owns only the window, raw input, the native menu's
widgets, file dialogs / drag-drop plumbing, fullscreen window styling, the
frame clock, and the presenter. Every decision about what input *means* is in
portable code.

## Resulting app API (`app.h`)

Called by platform layers:

- `Init`, `FrameUpdate`, `FrameRender`, `FrameResize` (unchanged)
- `AppInvokeCommand(arena, id)` (native menu click)
- `AppCommandState(arena, id) -> CommandState {enabled, checkable, checked}` (native menu validation; replaces `AppCommandContext`)
- `AppAcceptsDroppedFile(path)` (drag cursor feedback)

Removed: `AppRequestRender`, `AppDebugHudVisible`, `AppCommandContext`,
`ImportBlendFile`. `FrameGpuTimings` stays for the parity harness only.

Contract changes:

- `FrameUpdate` with `deltaTime == 0` means the loop just started or resumed;
  the app forces a few rendered frames itself. `Init` forces the first frames.
- `FrameInput` gains `mouseDeltaX/Y` (pointer motion in points),
  `displayRefreshHz`, and `openedFile` (a path picked in the open dialog, valid
  for one frame).
- `PlatformMenuHooks.importFile(context)` becomes
  `showOpenDialog(context, extension)`; the pick arrives as
  `FrameInput.openedFile`.

## Steps

1. **Render requests** — `Init` forces 3 frames; `deltaTime == 0` forces 3
   frames; drop every platform `AppRequestRender` call (resize / command
   already force). Test: parity passes; app still paints after restore.
2. **Command state** — `CommandQueryState` in `menu.cpp`, `AppCommandState` in
   `app.cpp`; Windows `UpdateMenuPopup` and mac `validateMenuItem` use it.
   Parity `--ao-off` cycles AO twice instead of poking flags.
3. **F3 chord + Escape** — app ORs `ShortcutMod_F3` while F3 is held and turns
   Escape-in-fullscreen into `Command_ToggleFullscreen` (unless a text field
   had the keyboard). Platforms stop tracking F3 / Escape.
4. **Camera drag** — platforms accumulate `mouseDeltaX/Y`; `FrameUpdate` maps
   right / shift+right / middle drag into pan / orbit.
5. **Import + drop policy** — `showOpenDialog(context, "blend")` from
   `menu.cpp`; `FrameUpdate` imports `input.openedFile`;
   `AppAcceptsDroppedFile` owns the `.wav` rule.
6. **In-app HUD** — app owns `FrameStats`, pushes a sample per rendered frame,
   refreshes the five readout lines at ~15 Hz, and `UiDrawStatsHud` draws the
   panel + graph at the viewport's top-right (the toolbar owns top-left).
   Delete `platform_windows_hud.*` and `DebugHudView`.
7. **Windows tidy** — monitor refresh query moves out of the GL presenter into
   `platform_windows.cpp` (it is not graphics work); `build.bat` drops the HUD
   file; header comments and `CLAUDE.md` updated.

## Verification

- `build.bat test` (unit + parity against goldens; HUD is off in every case).
- Manual on Windows: F3 tap (HUD), F3+O/F/B, Ctrl+F then Escape, right-drag,
  shift+right-drag, middle-drag, drop a `.wav` (and a non-wav: no-drop cursor),
  File > Import File..., Alt+F4, minimize/restore.
- macOS edits cannot be compiled here: run `make && make parity-test` on a Mac.
