<img alt="Bite DJ logo - vert color on dark" src="https://github.com/user-attachments/assets/7d24793a-7660-4c75-906b-6f83c201619f" />

By Deckshark

# BiteDJ

BiteDJ is an independent project based upon the free and open source Mixxx DJ software and is distributed under the GNU General Public License v2.0. Support requests regarding BiteDJ should be directed at its maintainers.

All changes in this fork were made from the upstream base revision **Mixxx 2.5.6**.

Bite DJ targets a fixed-function, embedded, linux-based, touchscreen-driven DJ unit with a USB-drive-centric library model. Many changes below exist to serve that appliance model — no keyboard, no mouse, no local music library, no modal dialogs, and a hard CPU budget.

## Community

* [Blog](https://www.deckshark.us/blogs/news)
* [Discord](https://discord.com/invite/WJw6vdZKwQ)

## Releases

The sections below describe the fork as a whole. In brief, by release:

- **1.0-3** — On Air load gate for playing decks, debug-log export to USB,
  recording and Browse hardening, Rekordbox import robustness, multi-partition
  drive LEDs.
- **1.0-2** — full FX parameter pane with beat-period controls, beat grid editor,
  beat grids and keys stored on the USB drive, CJK fonts, drive-playing LEDs.
  Plus community contributions: elapsed/remaining time mode, CDJ-style browser
  navigation, reorderable library columns, Rekordbox HFS+ detection, hidden ALSA
  logical devices.
- **1.0-1** — audio-path resilience (device-loss watchdog, USB-dropout recovery,
  RT priorities for the disk path), EQ/Isolator modes, USB-only history, WAL
  library database, Latency/Quality preset. Plus community contributions: memory
  cue controls, CFX Filter effect, Inter UI font, per-user removable-media roots.
- **1.0** — initial release.

## Credits

- Pablo Feijo's [Custom Bite DJ](https://github.com/pablo-feijo/custom-bitedj) —
  FX pane parameter metadata (`_is_mix`, `_beat_period*`, `<ShowParameterValue>`),
  recording, Browse and Rekordbox hardening.
- Tyler "subsect"'s PiFlex — origin of the beat grid editor; engine, library and
  Rekordbox ANLZ/PDB fixes.
- Kyohei Okuno — memory cue controls, CFX Filter, time mode switching, CDJ-style
  browser navigation, column reordering, and several fixes.

---

## Rebrand

- Renamed Mixxx → **Bite DJ** throughout the user-facing product: binary `bitedj`,
  application id `us.deckshark.BiteDJ`, data directory `~/.bitedj`, effect ids
  `us.deckshark.effects.*`. The C++ `mixxx::` namespace, controller scripting API,
  and upstream copyright/attribution are deliberately unchanged (GPLv2).
- The fork is versioned independently of upstream: `BITEDJ_VERSION` drives the
  product name and version everywhere the user sees them, while the Mixxx base
  version still keys the config upgrade path and the manual URLs.
- Removed "mixxx" from the system version info string; added a version label widget
  (`WVersionLabel`) and firmware release info to the system settings page. The
  firmware version is read from `VERSION=` in `/etc/os-release`.

## Platform & build

- Pruned all sources and build configuration unrelated to Linux — Windows, macOS and
  iOS packaging, `nativeeventhandlerwin`, Apple libs, WiX installer, and the
  associated build scripts.
- Stripped QML (`src/qml`, `res/qml`) and Qt PrintSupport from the build to save
  image space.
- Removed all upstream default skins; the BiteDJ theme is the only skin. It began
  as a git submodule and was later flattened into a normal directory tree in
  `res/skins`, since it is effectively the default theme now.
- Added an Arch Linux build environment script and a documented aarch64
  cross-build script.
- GitHub workflow runs the unit tests on every PR.
- Committed the upstream lint/format configs, an interactive merge-conflict
  resolver, and `tools/resolve-branding.sh`.
- The Debian package depends on `fonts-noto-cjk`.
- Fixed all `-Werror` warnings.

## Real-time & performance

- Native CPU pinning and `rtprio` support (`src/util/rtscheduling`); the main thread
  now runs with real-time priority. Note that plain `QThread::start()` inherits the
  RT policy, so worker threads must pass an explicit priority.
- The two hops between the audio callback and the disk run at real `SCHED_FIFO`
  priorities; analysis is demoted to `SCHED_BATCH`/nice 19 with the weakest
  best-effort I/O priority. (QThread priorities are no-ops under `SCHED_OTHER`,
  so waveform rendering could otherwise preempt the fetch of samples about to be
  played.)
- Gave the audio engine thread a unique name.
- Default `KeylockEngine` forced to SoundTouch — RubberBand's phase vocoder is too
  expensive on the target hardware.
- Idle CPU reduction: paused decks no longer run the full DSP chain over silence, and
  waveforms no longer repaint at 60 fps when static.
- Minimum analyzer thread count raised to two.
- Reduced latency on the track-list jog wheel; selection changes now do a cache-only
  track lookup (`GlobalTrackCache` + `TrackRef`) instead of a full `getTrack()`, with
  the real load deferred to deck load.
- Underruns are counted in the callback and logged in aggregate, with the network
  stream reported separately from the sound card.
- `ShoutConnection`s are created only while broadcasting is on and the profile is
  enabled, and no default profile ships (the target kernel has no IP stack).
- External-library models (Rekordbox, Serato, …) look up a track's row through a
  location hash built on `select()` instead of a linear scan.

## Audio path resilience

- **Device-loss watchdog** in `SoundManager`: PortAudio never reports device removal,
  so stream liveness and per-device callback ticks are polled and a dead device is
  torn down (`outputDeviceLost()`). `AudioDeviceSettings` retries every 3 s until the
  configured device returns; `SoundManagerConfig::relinkDeviceIds()` re-matches the
  device by name since its PortAudio index and ALSA `hw:X,Y` change on re-plug.
- **USB dropout recovery**: a short or empty read no longer shrinks a track's
  readable range for good (which left the deck silent while the transport kept
  moving). `CachingReaderWorker` re-opens the file in place and retries, shrinks the
  range only when a fresh decoder stops at the same frame, and ejects the track only
  if reads keep failing past a time limit. Read failures name the deck in the log.
- When the reader's chunk request FIFO is full, `CachingReader` wakes the worker and
  stops hinting instead of logging once per chunk from the audio callback.
- Fixed `EngineEffectChain`'s dry-plus-wet mix writing one sample past the end of the
  buffer.
- ALSA logical PCMs (`default`, `pulse`, `dmix`, surround aliases) are hidden from
  the device list; only direct hardware devices are offered.

## Touchscreen UI

- Removed the mouse/keyboard interaction model: dialog boxes globally removed, menu
  bar auto-hidden at start, tooltips globally removed. (The preferences dialog was
  later restored for service use.)
- Added an inline notification strip (`Notifications`, `WNotificationStrip`) that
  replaces warning dialogs, including a busy state with sticky publish. Sound-device
  dialogs route through it while preserving config. Error text is no longer clipped
  at the start of the message bar. `Notifications::publishWithAction()` lets the
  strip offer a tappable action; tapping elsewhere, the 5 s auto-clear, or any later
  message drops the action without running it.
- Auto-focus library widgets and repurpose the wheel push.
- Library sidebar hides itself when a leaf node (playlist) is tapped.
- Track browser made responsive to touch and drag (`TouchScrollFilter`). A double tap
  in a track list no longer loads the track (too easy to do by accident); Return and
  controller `GoToItem` still load. A finger left down after a double tap now
  scrolls instead of dragging out a selection.
- `ContentDragScroller` provides drag-scrolling for in-skin panes.
- Top tab buttons and the settings sub-tab bar changed from toggles to single-shot
  buttons.
- `WPushButton` gains `<Repeat>`, with optional `<RepeatDelay>`/`<RepeatInterval>`:
  holding a button repeats its action, accelerating to a floor.
- Screen rotation support, moved in settings from USB to Power so it is not pressed
  by accident, and condensed into a shared row with power controls.
- **High-contrast mode** for daylight use (`src/skin/highcontrast`,
  `ImgHighContrast`), including inverted Q icon, light BEAT FX dropdown, and a
  dropdown arrow asset.
- **CJK text**: Settings → Library → CHARS (AUTO / JP / SC / TC / KR) inserts the
  matching Noto Sans CJK regional font after the first font in every skin
  font-family list and sets it as the application font (`CharacterPriority`). AUTO
  uses the UI locale and falls back to JP. Changes rebuild the skin after a 700 ms
  debounce.
- Global UI font changed from Monospace to Inter.
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
  3s cooldown and renders the "applying settings" message pre-emptively. A
  **Latency/Quality** preset moves sample rate and buffer size together
  (44.1 kHz/256 frames vs 48 kHz/1024 frames).
- Audio/Device changes are **staged behind an explicit Apply**, and taps queued during
  apply freezes are dropped. Rescan and Apply are greyed out while any deck is
  playing, to prevent audio dropouts.
- **General** page: **On Air** setting (`[BiteDJ],load_into_playing`) — Allow | Warn
  | Block — for loading onto a playing deck (see Decks). Deck 1/2 crossfader
  assignment is a single Xfader row with two tap-to-cycle A / THRU / B buttons.
- **EQ** settings page, later folded into a new top-level **Levels** page with knobs
  for booth, master and headphone levels.
- **EQ / Isolator mode** (`[BiteDJ],eq_mode`): one global choice of how a turned-down
  knob cuts. EQ bottoms out at a finite shelf; Isolator (the default) reaches silence
  at the stop. The curves cross at half travel. Honoured by the deck EQs, the master
  graphic EQ and the waveform tint. The deck EQ parameter now uses a linear taper,
  which also puts MIDI 64 exactly on the detent.
- **Library** page fits the 480×420 panel: each column has one button that cycles
  OFF → XS → S → M → L (`[Library],column_size_<name>`); hiding the last visible
  column is refused.
- **USB** drive management page with per-drive eject, power off, a scrollbar, and a
  live-updating drive list. Devices view also gained a scrollbar.
- **System** page with firmware release info and logging configuration.
- Key notation, waveform style, tempo range percent, filtered-waveform EQ
  (`ApplyEqToWaveform`), vinyl brake, cue gating, and elapsed/remaining time mode
  are all now settable from the UI.
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
- Removable media is also detected under per-user automount roots
  (`/media/$USER`, `/run/media/$USER`).
- Tracks are evicted from the global cache on eject so the OS unmount succeeds
  (release is asynchronous — the reader worker's FD close and cache eviction are both
  queued, so the unmount must be retried after pumping the event loop).
- Analysis cache split to be per-filesystem based on track file location
  (`FsAnalysisCache`), with the home-directory cache disabled by default; the
  filesystem cache is unloaded when its drive is ejected.
- **Beat grids and detected keys live on the drive** in `FsAnalysisCache`, next to
  the waveforms. The home library table keeps only the BPM float and key text for
  browsing and search, so `mixxxdb.sqlite` stays small enough to wipe on firmware
  updates without losing analysis. Read-only drives skip the save quietly.
- New per-drive stores under `.bitedj/` on the stick: cue overrides
  (`FsCueOverrideStore`), track metadata/ratings (`FsMetaOverrideStore`), play history
  (`FsHistoryStore`), and sampler banks (`FsSamplerBankStore`), all on a shared
  `FsStore` base.
- Cue points and ratings set in Bite DJ persist and override those from other
  libraries, across USBs.
- Sidebar reworked for the appliance: dropped `/` from Computer, dropped Recordings
  and Analyze.
- **History is USB-only**: sets live on the drive they were played from. Local
  setlogs were dropped entirely and those left by earlier versions are purged. All
  history reads and writes run on `FsHistoryWorker`'s own thread (a single append to
  a USB stick has been measured at over three seconds).
- The library database runs in **WAL mode**, and Rekordbox and Serato scans read the
  drive fully before opening their write transaction, so opening a playlist no
  longer freezes the UI during a scan.
- Rekordbox/Serato sidebar entries are hidden while no matching USB device is mounted.
  Devices stay hidden until all of their playlists have loaded, and all playlists for
  a device appear at once.
- Drive polling raised to 5s with a background poller on `RekordboxFeature`; drives
  are removed after 3 consecutive empty scans.
- Fixed stale Rekordbox device identity: devices were keyed by mount name rather than
  volume, so a reused name showed the previous drive's library, and stale playlist
  rows broke re-plugged devices.
- External drive references are removed from the track browser when the filesystem is
  ejected, and an open playlist closes back to the sidebar browser on eject.
- **Drive LEDs**: `[BiteDJ],drive<N>_playing` (read-only) is 1 while a track from any
  partition of the drive in USB port N is playing in a deck, sampler or preview deck.
  The hidden `bitedj.midi.xml` mapping sends it to the eject daemon, which flashes
  that port's LED. `PortMidiController` is marked open before its mapping is applied
  so the first output values are actually sent.
- **Debug-log export**: tapping a drive's name three times within a second in
  Settings → System → USB Drives writes `/var/log` to the drive root as
  `bitedj-<firmware>-<timestamp>-debug.tar.gz` (script in
  `res/support/debuglogexport.xml`). It is written under a hidden `.partial` name and
  renamed when complete, runs niced off the GUI thread's `SCHED_FIFO` priority in its
  own process group, works with BusyBox tar, and is cancelled and cleaned up if the
  drive is ejected.

## Rekordbox & Serato import

- Backported upstream HFS+ Rekordbox database detection (mixxxdj/mixxx#16895).
- A truncated or corrupt ANLZ file no longer fails the track load: beats and cues are
  read in separate passes that each catch their own parse error, and the notification
  strip says some analysis could not be read.
- Exports without an `.EXT` file now get their beat grid from the `.DAT`.
- Tracks may share one ANLZ file (`analyze_path` is no longer UNIQUE); a track whose
  local row can't be found is skipped with a warning.
- `export.pdb` page chains are checked against the file size and for cycles, and
  table page indices are no longer truncated to 16 bits. The playlist tree skips a
  folder that is its own ancestor.
- New indexes on `(rb_id, device)` and `(playlist_id, position)` make import lookups
  no longer quadratic under the database write lock.

## Track browser

- `LibraryColumnControl` for in-skin column visibility and width, with self-healing
  layout and an enforced minimum width for the rating column. Columns can be
  reordered by dragging the header; order is persisted per model, while Bite DJ's
  visibility and widths stay authoritative.
- `BrowseTableModel` columns tagged with `kHeaderNameRole`; Filename column hidden by
  default; Duration renamed to Time; Key column honors `[Library],key_notation` and
  sorts correctly on external libraries.
- `BrowseThread` can no longer lose a folder tap; a per-request generation discards
  stale rows from the previous folder, and rows arrive in batches of up to 100 (or
  every 100 ms) through a bounded queue rather than with a 20 ms sleep per file.
- Missing tracks are colored red in the browser and can only be clicked once; the
  "file not found" filename is truncated to 32 chars to stay readable on a small
  screen. A valid loaded track is no longer cleared from a deck when the incoming
  track's file is missing.
- Already-played tracks are colored blue (`PlayedTracks`).
- BPM displayed to one decimal; BPM-locked indicator removed from the browser to
  reduce clutter; BPM now shown alongside key in the deck widget-group summaries.
- Cover art plumbing removed from the track model.
- Track loading is disabled when the library page is not the active page.
- The currently-playing track is identifiable from the track overview when switched
  to the browser.
- Fixed a segfault when hovering the "search related tracks" context menu item.
- Computer-tree leaf collapse fixed for subdirectories containing only hidden entries.

## Decks, cues & jog

- **On Air load gate** (`[BiteDJ],load_into_playing`): Allow (stock behaviour — the
  deck stops and the new track loads), Warn (the load is held until the DJ taps LOAD
  on the notification strip before it times out), or Block. Only decks are gated;
  samplers and the preview deck load as before. Default is Allow.
- **Vinyl / CDJ jog mode** toggle in the UI, with configurable vinyl brake. Brake
  deceleration is proportional to platter speed and decelerates to the speed of a
  playing track. Cues out-rank the brake. Brake settings are disabled while CDJ mode
  is active. Scratching now ends when play/pause is pressed.
- Jog wheel filter length is configurable via `[Controls],JogWheelFilterLength`; the
  default drops from 25 to 6 for more direct jog response.
- **Hot cues and memory cues separated**: looping hot cues actually loop, memory cues
  are exposed in the UI and auto-play on cue. Cue gating (gated vs. ungated) is
  user-configurable. Added a clear button on cue pads; cues clear from the UI
  immediately on Settings → Clear → Cues, without clearing Rekordbox-authored cues.
- **Memory cue controls**: Pioneer-style set / delete / prev / next navigation over a
  10-slot memory cue bank (stored in reserved hotcue slots, so the cue DB format is
  unchanged), mapped on the DDJ-400, FLX2 and FLX4. The hot cue bank grows to 16
  pads (rekordbox A–P).
- **Time mode**: tap the time display to switch between elapsed and remaining; the
  choice persists. Remaining is the default. The Play page shows REMAIN / TIME
  indicators, non-Play pages show track time in the deck header, and all deck times
  use tabular digits.
- **Beat grid editor**: a grid page in the Overview panel for nudging and stretching
  a deck's beat grid, with hold-to-repeat 10 ms / 0.01 BPM steps. "Set" is renamed
  "Align".
- **Per-USB, per-deck samplers** (`SamplerDrive`, `WSamplerDrive`) — sampler decks are
  loaded and saved on a per-USB basis.
- Volume faders start at 0 instead of 100%.
- Loop-in button blinks while a loop is being created.
- Jog-wheel rotation on the play tab zooms the waveform without holding shift; maximum
  waveform zoom increased 16×; white bars are removed from the waveform in proportion
  to zoom so they don't overwhelm it.
- Main waveform touch no longer seeks (`setSeekDisabled`).
- Key display highlights orange on change in the waveform overview.
- The waveform overview uses skin-defined hot cue labels.

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
- The FX pane shows **every parameter** of the loaded effect, with separate knob and
  button row templates, and drag-scrolls. The internal dry/wet row is hidden on skins
  with a unit Mix knob (`parameterN_is_mix`), keeping its value and Super linkage.
- `parameterN_beat_period` / `_beat_period_min` / `_beat_period_max` give Beats-typed
  parameters a common "period in beats" unit, converting to each effect's native
  encoding and reporting the value the engine actually clamped to.
- New **CFX Filter** effect.
- Per-effect parameters persist across BeatFX switches.
- FX pane split into FX and Pitch, with pitch up/down in the UI; later split again
  into Key vs. FX sub-panels, with tests for keyfx. Fixed slight row expansion when
  switching between the FX and KEY sub-panes.
- Fixed effect knob parameter changes to use `slotValueChanged`.
- `EffectSlot::setEnabled()` now reaches the DSP when called from code (e.g. the EQ
  bypass preference).

## Controllers & MIDI

- **Soft takeover indicator** with a directional arrow (`SoftTakeoverIndicator`,
  `WSoftTakeoverIndicator`), suppressed below an 8% threshold. Soft takeover extended
  to all trim, EQ and channel-fader controls on the FLX4.
- New/updated mappings: DDJ-400, FLX2, the Deckshark virtual MIDI controller, and the
  Takeout box.
- FLX4/FLX2/DDJ-400: BEAT FX left/right step between buckets of quantized FX
  parameters rather than switching FX slots; buckets use the beat-period controls,
  so ⅛…4 land on the same beat length for every effect. Fixed the FLX4 FX enable
  button and channel 1 & 2 BEAT FX select. DDJ-400 Beat FX targets Effect1 directly
  (fixing FX channel buttons going dark when master was selected for FX). Top four
  beat-loop buttons changed from loops to rolls, and tempo range options widened.
- **CDJ-style browser navigation** on DDJ-400, FLX2 and FLX4: push encoder opens
  Browse from Play, Shift+push goes back from playlist to sidebar, loading a track
  returns to Play, Shift+Load toggles the browser.
- `Pm_Initialize` deferred and MIDI inputs deduplicated.
- Rescan automatically re-enables any valid controllers; fixed a crash when applying
  preferences after a controller appeared only on a post-startup rescan.
- Booth output hidden in sound preferences unless `[Deckshark],show_booth_output`.

## Recording

- **Recording to USB**, with a recording dot on the settings tab while active.
- Recording onto a drive that a playing deck is reading from is refused; if a track
  later starts playing from the recording drive, a one-time warning is shown.
- Encoder tell/seek/filelen are 64-bit, fixing WAV headers of takes past 2 GiB (and
  the `SEEK_END` sign in `EncoderWave`).
- `EngineRecord` detects write, seek, flush and encoder-init failures, finalizes what
  reached the drive and stops with an error. Errors and low-space warnings go to the
  notification strip, and an existing recording file name is never reused.
- The sidechain FIFO grows from 64k to 1M samples (~11.9 s) to ride out slow USB
  writes; `PageCacheLimiter` was removed, as its `sync_file_range` blocked and
  overflowed the recorder.

## Power

- Power-off from the UI, with a "shutting down" message.

## Tests

New test coverage added alongside the fork's features: `audiosourceshortread`,
`browsethread`, `characterpriority`, `contentdragscroller`, `driveplaying`,
`effectbeatperiod`, `eqmode`, `foldertreemodel`, `fscueoverridestore`,
`fshistorystore`, `fshistoryworker`, `fsmetaoverridestore`, `fssamplerbankstore`,
`highcontrast`, `hotcuecontrol` (memory cues), `keycontrol`, `librarycolumncontrol`,
`playedtracks`, `rekordboxanlz`, `rotary`, `samplerdrive`, `systemsettings`,
`touchscrollfilter`, `waveformmarkset`, `wcontrollerlist`, `wnumberpos`,
`wpushbutton`, and `wusblist`.

## Misc

- Skins can mark the beat spin box read-only; `BindProperty` falls back to dynamic
  `QWidget::setProperty`; arbitrary sidebar entries can be hidden.
- `WLabel` supports optional tabular numbers.
- Slot loaded/unloaded log messages raised to INFO severity.
- Fork settings `shared_ptr`s are dropped before manager teardown.
- It will never be Christmas 2024 again.
