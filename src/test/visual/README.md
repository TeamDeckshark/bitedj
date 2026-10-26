# Screenshot tests

`bitedj-visual-test` starts the whole application headless (Qt's `offscreen`
platform, a fresh settings directory, every real USB mount hidden behind an
empty tmpfs) at a scenario's window size, drives it through the scenario's
steps and compares grabs of the main window against the golden images in
`golden/`.

    make -C build bitedj-visual-test
    build/bitedj-visual-test src/test/visual/grid_analysis_source.json
    (cd build && ctest -L visual --output-on-failure)

A failing snapshot writes `<name>.png` (the grab) and `<name>.diff.png`
(differing pixels in red) to `--out` (default `$TMPDIR/bitedj-visual-test/`,
under CTest `build/visual-test-output/`).

## Adding a scenario

1. Write `<scenario>.json` here (format in `visualtestrunner.h`). Steps:
   `load`, `waitFor` (a control reaching a value), `waitForAnalysis`, `set`,
   `sidebar` (walk the library sidebar by label and tap the last one, or
   expand it with `"expand": true`; `timeout` waits for items found in the
   background), `selectRow` (select the track-table row showing a text),
   `textColor` (assert the colour a track-table cell's text is painted in,
   e.g. `{"textColor": "Same Key 8A", "column": "Title", "color": "#6ee128"}`;
   use it next to a snapshot so a colour change names the row that broke),
   `click` (a visible button by object name and text prefix), `wait`,
   `resize`, `snapshot` (optional `masks`, `tolerance`, `maxDiffPixels`).
   `rekordbox` makes files count as rekordbox-analysed. `drives` mounts fake
   USB sticks at `/media/<name>` (a tmpfs each) holding the listed fixture
   files; `drives/serato/` is a Serato library made by its `make_fixture.py`.
   `config` seeds `bitedj.cfg`; `{scenario}` in a value is this directory,
   so `"[Browse],QuickLinks": "{scenario}/media/"` puts the tagged fixtures
   in `media/` under Computer -> Quick Links for Browse to show.
2. Run it with `--update`, look at every grab it wrote to `golden/`, and only
   commit them once they show what the screen should look like.
3. Re-run without `--update` a few times: it must pass with 0 differing
   pixels. If something moves between runs, wait for the state it depends
   on rather than raising the budget.

Grabs are pixel-identical from run to run on one machine, so the default
budget is zero pixels past a per-channel tolerance of 16. Fonts and Qt
versions change the rendering: regenerate the goldens (and review them) after
a Qt or font upgrade, or when the screens change on purpose.

The window is forced to the scenario's size even when the skin's layout wants
more (the runner says so): a screen that small cuts the skin off, and the
grab must show that. A playing deck's clock and overview move from run to run: mask them (see
`harmonic_match.json`) rather than freezing the deck in an odd state. GL
waveforms do not render offscreen; trust the play
area for what is drawn and where, not for the waveform itself.
