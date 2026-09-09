# Timeline tracks derived from audio sources

## Problem

Timeline tracks are currently a standalone pool: a track only appears when a
`.wav` is dropped past the last lane, `TimelineAddTrack` / `TimelineRemoveTrack`
push undo commands, and `TimelineTrack` carries its own `name` / `muted` /
`soloed`. There is no track for a source until audio lands on it, and the track
header is a static label plus two non-interactive M/S glyphs.

Wanted: one track per scene audio source, always, in source order. The track
header is a rectangle on the left of the lane holding the source's name (edit it
to rename the source), a volume slider (0..1, bound to `AudioSourceParams.gain`),
and working mute / solo buttons.

## Decisions

- Tracks are **derived 1:1 from audio sources**, not stored. `TrackId` becomes
  an alias for the source `EntityId`. No track pool, no per-track undo. A clip
  whose source entity is gone is dropped (not undoable).
- Name sync is **two-way**: the header label always shows the entity name;
  committing an edit calls `SceneRenameEntity` (one undo step, same as the
  outliner).
- Volume slider range **0..1**, writes straight to `AudioSourceParams.gain`
  via `SceneFindAudioSourceParams` (same mutable-inspector path the properties
  panel uses; not undoable).
- Mute / solo buttons write **directly to `AudioSourceParams`**. The old
  per-frame mirror of `TimelineTrack.muted/soloed` into the params is removed.

## Steps

### 1. timeline model (`src/timeline.h` / `.cpp`)

- `typedef EntityId TrackId;` keep `kInvalidTrackId = 0`.
- Redefine `TimelineTrack` as a per-query view: `{ EntityId source; char
  name[64]; float gain; bool muted; bool soloed; }`.
- `TimelineTracks(timeline, scene, out, maxOut)` — new `scene` param; one row
  per `SceneAudioSources` entry, `id` = source entity, fields copied from the
  entity name + `params`.
- Delete `TrackSlot`, `TimelineState::tracks`, `FirstFreeTrack`,
  `FindTrackSlotConst`, `TimelineFindTrack`, `TimelineAddTrack`,
  `TimelineRemoveTrack`, the `TL_AddTrack` / `TL_RemoveTrack` command kinds and
  their payload fields.
- `TimelineUpdate` — drop the mute/solo mirror loop; instead free any used clip
  whose `clip.track` is no longer a live audio source (clear selection entry,
  `used = false`).
- `TimelineAddClip` / `TimelineMoveClip` — validate the target track with
  `SceneFindAudioSourceParams(scene, track) != nullptr`.
- `CollectVoices` — `voice.source = clip.track` directly; compare `clip.track`
  for the per-source filter.

### 2. timeline panel (`src/ui.cpp`)

- `kTimelineHeaderWidth` 150 -> 184 for two rows of controls.
- `TimelineHeaderSlider` + `TimelineHeaderToggle`: compact immediate-mode
  widgets sized to a 14 px row (the existing `Slider` is 46 px tall).
- `DrawTimelineLanes` -> `BuildTimelineLanes(ui, scene, layout)`:
  - header rect, then row 1 = editable name (double-click, `SceneRenameEntity`),
    row 2 = `[volume slider][M][S]` writing `params->gain/mute/solo`.
- `BuildTimelineBody` passes `scene` to `TimelineTracks`.
- `HandleTimelineDrop`: target track = lane's source, else
  `SelectedAudioSource(scene)`; if still invalid, skip the file (no track can be
  created without a source).

### 3. test (`tests/timeline_undo_test.cpp`)

- Replace `TimelineAddTrack` calls with `SceneCreateEntity(..., AudioSource)`;
  `TrackCount` now takes `scene`; a track exists as soon as the source does; the
  clip-undo chain is clip -> entity (no track step).

## Verify

```
make run
```

Default scene has two audio sources -> two timeline tracks on launch. Each lane
header shows the source name; double-click to rename (viewport + outliner
follow). Drag the volume slider, toggle M / S -> audible change. Add a Source in
the outliner -> a third track appears; delete it -> its track and clips go.

Standalone test:

```
clang++ -std=c++17 -Wall -Wextra -I src tests/timeline_undo_test.cpp \
  src/timeline.cpp src/scene.cpp src/arena.cpp src/undo_stack.cpp \
  -o build/timeline_undo_test && build/timeline_undo_test
```
