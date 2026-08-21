# Implementation Plan

## Goal 1: Video Original Audio
Currently, `VideoSource` extracts proxy images and a low-quality `audio.f32` file for the waveform display, but does not load the full PCM audio for live playback.
I will:
1. Update `Clip` to include a `volume` field (default 1.0f).
2. Modify the background `BuildProxy` process to also extract `audio.wav` (48kHz stereo) using `ffmpeg`.
3. Add a `std::shared_ptr<Song> audio;` to `VideoSource`, which gets populated from `audio.wav` using the existing `DecodeSongFileUncached` function.
4. Modify `AudioCallback` to iterate over `g_clips` and `g_over`, mixing the `audio` from any video clips that have `useAudio = true` and `volume > 0`.
5. Update `ExportFilm` to apply the `volume=...` filter to video audio streams during export.
6. Add a volume slider to the clip properties panel for video clips.
7. Update `ProjectToText` and `LoadProjectFromText` to serialize the new `volume` field.

> [!WARNING]
> Decoding the entire audio track of large video files into memory (as `DecodeSongFileUncached` does for songs) will use ~1.3 GB of RAM per hour of video. Is this acceptable, or should we implement on-the-fly streaming for video audio?

## Goal 2: Fold Multiple Tracks into a Sequence
Currently, `FoldSelection` only works for a contiguous block of clips on the base track.
I will upgrade `FoldSelection` to encompass clips from `g_clips`, `g_over`, and `g_atracks`:
1. Calculate the bounding box of all selected clips.
2. If base clips are selected:
   - They must still be contiguous.
   - The sequence duration will be forced to the duration of the selected base clips to prevent shifting unselected base clips and breaking sync.
   - The new `Nest` clip will replace them on the base track.
3. If no base clips are selected:
   - The sequence duration will be the full bounding box of the selected overlay/audio clips.
   - The new `Nest` clip will be placed on an overlay track (creating a new one if necessary) starting at the bounding box minimum time.
4. Move all selected clips (base, overlay, and audio blocks) into the newly created `Sequence`.
5. Offset their start times inside the sequence relative to the sequence's start time on the parent timeline.

> [!IMPORTANT]
> If you select base clips and overlay clips, but the overlay clips are *longer* than the selected base clips, the resulting `Nest` clip on the base track will only be as long as the base clips. The longer overlay clips will be cut off visually unless you expand the nested clip's duration later. Is this the behavior you expect?
