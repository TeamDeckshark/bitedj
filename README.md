<img alt="Bite DJ logo - vert color on dark" src="https://github.com/user-attachments/assets/7d24793a-7660-4c75-906b-6f83c201619f" />

By Deckshark

# BiteDJ

BiteDJ is an independent project based upon the free and open source Mixxx DJ software and is distributed under the GNU General Public License v2.0. Support requests regarding BiteDJ should be directed at its maintainers.

All changes in this fork were made from the upstream base revision **Mixxx 2.5.6**.

Bite DJ targets a fixed-function, embedded, linux-based, touchscreen-driven DJ unit with a USB-drive-centric library model. Many changes below exist to serve that appliance model — no keyboard, no mouse, no local music library, no modal dialogs, and a hard CPU budget.

## Community

* [Blog](https://www.deckshark.us/blogs/news)
* [Discord](https://discord.com/invite/WJw6vdZKwQ)

---

## Rebrand

- Renamed Mixxx → **Bite DJ** throughout the user-facing product: binary `bitedj`,
  application id `us.deckshark.BiteDJ`, data directory `~/.bitedj`, effect ids
  `us.deckshark.effects.*`. The C++ `mixxx::` namespace, controller scripting API,
  and upstream copyright/attribution are deliberately unchanged (GPLv2).
- Removed "mixxx" from the system version info string; added a version label widget
  (`WVersionLabel`) and firmware release info to the system settings page.

## Platform & build

- Pruned all sources and build configuration unrelated to Linux — Windows, macOS and
  iOS packaging, `nativeeventhandlerwin`, Apple libs, WiX installer, and the
  associated build scripts.
- Stripped QML (`src/qml`, `res/qml`) and Qt PrintSupport from the build to save
  image space.
- Removed all upstream default skins; the BiteDJ theme is the only skin. It began
  as a git submodule and was later flattened into a normal directory tree in
  `res/skins`, since it is effectively the default theme now.
- Added an Arch Linux build environment script.
- Fixed all `-Werror` warnings.

## Real-time & performance

- Native CPU pinning and `rtprio` support (`src/util/rtscheduling`); the main thread
  now runs with real-time priority. Note that plain `QThread::start()` inherits the
  RT policy, so worker threads must pass an explicit priority.
- Gave the audio engine thread a unique name.
- Default `KeylockEngine` forced to SoundTouch — RubberBand's phase vocoder is too
  expensive on the target hardware.
- Idle CPU reduction: paused decks no longer run the full DSP chain over silence, and
  waveforms no longer repaint at 60 fps when static.
- Minimum analyzer thread count raised to two.
- Reduced latency on the track-list jog wheel; selection changes now do a cache-only
  track lookup (`GlobalTrackCache` + `TrackRef`) instead of a full `getTrack()`, with
  the real load deferred to deck load.

## Touchscreen UI

- Removed the mouse/keyboard interaction model: dialog boxes globally removed, menu
  bar auto-hidden at start, tooltips globally removed. (The preferences dialog was
  later restored for service use.)
- Added an inline notification strip (`Notifications`, `WNotificationStrip`) that
  replaces warning dialogs, including a busy state with sticky publish. Sound-device
  dialogs route through it while preserving config. Error text is no longer clipped
  at the start of the message bar.
- Auto-focus library widgets and repurpose the wheel push.
- Library sidebar hides itself when a leaf node (playlist) is tapped.
- Track browser made responsive to touch and drag (`TouchScrollFilter`).
- Top tab buttons and the settings sub-tab bar changed from toggles to single-shot
  buttons.
- Screen rotation support, moved in settings from USB to Power so it is not pressed
  by accident, and condensed into a shared row with power controls.
- **High-contrast mode** for daylight use (`src/skin/highcontrast`,
  `ImgHighContrast`), including inverted Q icon, light BEAT FX dropdown, and a
  dropdown arrow asset.
- Fixed white-background artifacts when changing pages, and the white waveform flash
  on tab switch (the GL window container's autofill palette before first buffer swap).

## Settings pages (in-skin)

The preferences dialog is not usable on a touchscreen, so most configuration moved
into the skin as dedicated pages backed by new settings objects
(`AudioDeviceSettings`, `ControllerSettings`, `SystemSettings`).

- **Devices** picker for MIDI and audio auto-route (`WControllerList`,
  `WAudioDeviceList`), with the ability to hide virtual controllers. Startup lands on
  Settings → Devices when no audio device or no MIDI controller is configured.
- **Audio** broken out into its own sub-page. Device selection is debounced with a
  3s cooldown and renders the "applying settings" message pre-emptively.
- Audio/Device changes are **staged behind an explicit Apply**, and taps queued during
  apply freezes are dropped. Rescan and Apply are greyed out while any deck is
  playing, to prevent audio dropouts.
- **EQ** settings page, later folded into a new top-level **Levels** page with knobs
  for booth, master and headphone levels.
- **USB** drive management page with per-drive eject, power off, a scrollbar, and a
  live-updating drive list. Devices view also gained a scrollbar.
- **System** page with firmware release info and logging configuration.
- Key notation, waveform style, tempo range percent, filtered-waveform EQ
  (`ApplyEqToWaveform`), vinyl brake, and cue gating are all now settable from the UI.
- Clear-cache controls: clear cached waveforms, and fully clear all analysis data —
  including the analysis databases on attached USB drives.
- Settings flush to disk after every UI change, with durable writes to `mixxx.cfg`.
  Note that the autosave rewrites the whole file from memory, so a small set of keys
  (`usb_drive_path_*`) are treated as file-authoritative.
- Empty audio output device is no longer persisted to `mixxx.cfg`.
- Music library path is no longer requested at startup when unconfigured.

## USB drives & library

The library is drive-first: a stick carries its own metadata, and nothing is expected
to live on the unit.

- USB drive management, eject, and power-off capabilities (`src/util/usbdevice`,
  `WUsbList`), including MIDI-triggered drive-level eject. Ejecting a drive ejects
  every filesystem belonging to it.
- Tracks are evicted from the global cache on eject so the OS unmount succeeds
  (release is asynchronous — the reader worker's FD close and cache eviction are both
  queued, so the unmount must be retried after pumping the event loop).
- Analysis cache split to be per-filesystem based on track file location
  (`FsAnalysisCache`), with the home-directory cache disabled by default; the
  filesystem cache is unloaded when its drive is ejected.
- New per-drive stores under `.bitedj/` on the stick: cue overrides
  (`FsCueOverrideStore`), track metadata/ratings (`FsMetaOverrideStore`), play history
  (`FsHistoryStore`), and sampler banks (`FsSamplerBankStore`), all on a shared
  `FsStore` base.
- Cue points and ratings set in Bite DJ persist and override those from other
  libraries, across USBs.
- Sidebar reworked for the appliance: dropped `/` from Computer, dropped Recordings
  and Analyze, and made played-track history USB-centric ("This Unit" plus per-drive
  history).
- Rekordbox/Serato sidebar entries are hidden while no matching USB device is mounted.
  Devices stay hidden until all of their playlists have loaded, and all playlists for
  a device appear at once.
- Drive polling raised to 5s with a background poller on `RekordboxFeature`; drives
  are removed after 3 consecutive empty scans.
- Fixed duplicate USB mount points in the sidebar (repeatedly — an empty-`$USER`
  `/media` double-scan across the browse/rekordbox/serato feeds, plus a udev coldplug
  double-mount in `deckshark-linux`).
- Fixed stale Rekordbox device identity: devices were keyed by mount name rather than
  volume, so a reused name showed the previous drive's library, and stale playlist
  rows broke re-plugged devices.
- External drive references are removed from the track browser when the filesystem is
  ejected, and an open playlist closes back to the sidebar browser on eject.
- **Recording to USB**, with a recording dot on the settings tab while active.

## Track browser

- `LibraryColumnControl` for in-skin column visibility and width, with self-healing
  layout and an enforced minimum width for the rating column.
- `BrowseTableModel` columns tagged with `kHeaderNameRole`; Filename column hidden by
  default; Duration renamed to Time; Key column honors `[Library],key_notation` and
  sorts correctly on external libraries.
- Missing tracks are colored red in the browser and can only be clicked once; the
  "file not found" filename is truncated to 32 chars to stay readable on a small
  screen. A valid loaded track is no longer cleared from a deck when the incoming
  track's file is missing.
- Already-played tracks are colored blue (`PlayedTracks`).
- BPM fraction component hidden; BPM-locked indicator removed from the browser to
  reduce clutter; BPM now shown alongside key in the deck widget-group summaries.
- Cover art plumbing removed from the track model.
- Track loading is disabled when the library page is not the active page.
- The currently-playing track is identifiable from the track overview when switched
  to the browser.
- Fixed a segfault when hovering the "search related tracks" context menu item.
- Computer-tree leaf collapse fixed for subdirectories containing only hidden entries.

## Decks, cues & jog

- **Vinyl / CDJ jog mode** toggle in the UI, with configurable vinyl brake. Brake
  deceleration is proportional to platter speed and decelerates to the speed of a
  playing track. Cues out-rank the brake. Brake settings are disabled while CDJ mode
  is active. Scratching now ends when play/pause is pressed.
- **Hot cues and memory cues separated**: looping hot cues actually loop, memory cues
  are exposed in the UI and auto-play on cue. Cue gating (gated vs. ungated) is
  user-configurable. Added a clear button on cue pads; cues clear from the UI
  immediately on Settings → Clear → Cues, without clearing Rekordbox-authored cues.
- **Per-USB, per-deck samplers** (`SamplerDrive`, `WSamplerDrive`) — sampler decks are
  loaded and saved on a per-USB basis.
- Volume faders start at 0 instead of 100%.
- Loop-in button blinks while a loop is being created.
- Jog-wheel rotation on the play tab zooms the waveform without holding shift; maximum
  waveform zoom increased 16×; white bars are removed from the waveform in proportion
  to zoom so they don't overwhelm it.
- Main waveform touch no longer seeks (`setSeekDisabled`).
- Key display highlights orange on change in the waveform overview.

## Analysis

- Drop detection during beat analysis, with downbeats anchored on detected drops;
  later tuned to select a single anchor and align every downbeat off it, assuming
  constant 4/4 tempo.
- Analyzed BPM is reconciled with the BPM in file/library metadata.
- In-progress analysis is aborted for tracks on a drive being ejected, and cancelled
  on replace when a new track is loaded into the same deck — the new load takes
  precedence.
- Waveform analysis is no longer prevented by transient electrical issues.

## Effects

- Per-parameter manifest metadata exposed as ControlObjects, plus per-effect-slot
  quantize/triplet proxies and a raw-value alias per knob parameter.
- Per-effect parameters persist across BeatFX switches.
- FX pane split into FX and Pitch, with pitch up/down in the UI; later split again
  into Key vs. FX sub-panels, with tests for keyfx. Fixed slight row expansion when
  switching between the FX and KEY sub-panes.
- Fixed effect knob parameter changes to use `slotValueChanged`.

## Controllers & MIDI

- **Soft takeover indicator** with a directional arrow (`SoftTakeoverIndicator`,
  `WSoftTakeoverIndicator`), suppressed below an 8% threshold. Soft takeover extended
  to all trim, EQ and channel-fader controls on the FLX4.
- New/updated mappings: DDJ-400, FLX2, the Deckshark virtual MIDI controller, and the
  Takeout box.
- FLX4: BEAT FX left/right now toggle between buckets of quantized FX parameters
  rather than switching FX slots; fixed the FX enable button and channel 1 & 2 BEAT FX
  select. DDJ-400: fixed both FX channel buttons going dark when master was selected
  for FX. Both: top four beat-loop buttons changed from loops to rolls, and tempo
  range options widened.
- `Pm_Initialize` deferred and MIDI inputs deduplicated.
- Rescan automatically re-enables any valid controllers; fixed a crash when applying
  preferences after a controller appeared only on a post-startup rescan.
- Booth output hidden in sound preferences unless `[Deckshark],show_booth_output`.

## Power

- Power-off from the UI, with a "shutting down" message.

## Tests

New test coverage added alongside the fork's features: `foldertreemodel`,
`fscueoverridestore`, `fshistorystore`, `fsmetaoverridestore`, `fssamplerbankstore`,
`highcontrast`, `keycontrol`, `librarycolumncontrol`, `playedtracks`,
`rekordboxanlz`, `samplerdrive`, `touchscrollfilter`, `waveformmarkset`,
`wcontrollerlist`, and `wusblist`.

## Misc

- Skins can mark the beat spin box read-only; `BindProperty` falls back to dynamic
  `QWidget::setProperty`; arbitrary sidebar entries can be hidden.
- Slot loaded/unloaded log messages raised to INFO severity.
- Fork settings `shared_ptr`s are dropped before manager teardown.
- It will never be Christmas 2024 again.
