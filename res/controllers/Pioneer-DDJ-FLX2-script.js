// Pioneer-DDJ-FLX2-script.js
// ****************************************************************************
// * Mixxx mapping script file for the Pioneer DDJ-FLX2.
// * Based on the Pioneer DDJ-400 mapping, which owners report works on the
// * DDJ-FLX2 hardware.
// * Authors: Warker, nschloe, dj3730, jusko, tiesjan
// * Reviewers: Be-ing, Holzhaus
// * Manual: https://manual.mixxx.org/2.3/en/hardware/controllers/pioneer_ddj_400.html
// ****************************************************************************
//
//  Implemented (as per manufacturer's manual):
//      * Mixer Section (Faders, EQ, Filter, Gain, Cue)
//      * Browsing and loading + Waveform zoom (shift)
//      * Jogwheels, Scratching, Bending, Loop adjust
//      * Cycle Temporange
//      * Beat Sync
//      * Hot Cue Mode
//      * Beat Loop Mode
//      * Beat Jump Mode
//      * Sampler Mode
//
//  Custom (Mixxx specific mappings):
//      * BeatFX: Assigned Effect Unit 1
//                < LEFT toggles focus between Effects 1, 2 and 3 leftward
//                > RIGHT toggles focus between Effects 1, 2 and 3 rightward
//                v DOWN loads next effect entry for focused Effect
//                SHIFT + v UP loads previous effect entry for focused Effect
//                LEVEL/DEPTH controls the Mix knob of the Effect Unit
//                SHIFT + LEVEL/DEPTH controls the Meta knob of the focused Effect
//                ON/OFF toggles focused effect slot
//                SHIFT + ON/OFF disables all three effect slots.
//      * Memory Cue CALL, MEMORY and DELETE (CUE/LOOP CALL arrows)
//      * Toggle quantize (Shift + channel cue)
//
//  Not implemented (after discussion and trial attempts):
//      * Loop Section:
//        * -4BEAT auto loop (hacky---prefer a clean way to set a 4 beat loop
//                            from a previous position on long press)
//
//      * Secondary pad modes (trial attempts complex and too experimental)
//        * Keyboard mode
//        * Pad FX1
//        * Pad FX2
//        * Keyshift mode

var PioneerDDJFLX2 = {};

PioneerDDJFLX2.lights = {
    beatFx: {
        status: 0x94,
        data1: 0x47,
    },
    shiftBeatFx: {
        status: 0x94,
        data1: 0x43,
    },
    deck1: {
        vuMeter: {
            status: 0xB0,
            data1: 0x02,
        },
        playPause: {
            status: 0x90,
            data1: 0x0B,
        },
        shiftPlayPause: {
            status: 0x90,
            data1: 0x47,
        },
        cue: {
            status: 0x90,
            data1: 0x0C,
        },
        shiftCue: {
            status: 0x90,
            data1: 0x48,
        },
    },
    deck2: {
        vuMeter: {
            status: 0xB0,
            data1: 0x02,
        },
        playPause: {
            status: 0x91,
            data1: 0x0B,
        },
        shiftPlayPause: {
            status: 0x91,
            data1: 0x47,
        },
        cue: {
            status: 0x91,
            data1: 0x0C,
        },
        shiftCue: {
            status: 0x91,
            data1: 0x48,
        },
    },
};

// Store timer IDs
PioneerDDJFLX2.timers = {};

// Jog wheel constants
PioneerDDJFLX2.vinylMode = true;
PioneerDDJFLX2.alpha = 1.0/8;
PioneerDDJFLX2.beta = PioneerDDJFLX2.alpha/32;

// Multiplier for fast seek through track using SHIFT+JOGWHEEL
PioneerDDJFLX2.fastSeekScale = 150;
PioneerDDJFLX2.bendScale = 0.8;

PioneerDDJFLX2.tempoRanges = [0.06, 0.10, 0.16, 0.25];

PioneerDDJFLX2.shiftButtonDown = [false, false];

// Jog wheel loop adjust
PioneerDDJFLX2.loopAdjustIn = [false, false];
PioneerDDJFLX2.loopAdjustOut = [false, false];
PioneerDDJFLX2.loopAdjustMultiply = 50;

// Beatjump pad (beatjump_size values)
PioneerDDJFLX2.beatjumpSizeForPad = {
    0x20: -1, // PAD 1
    0x21: 1,  // PAD 2
    0x22: -2, // PAD 3
    0x23: 2,  // PAD 4
    0x24: -4, // PAD 5
    0x25: 4,  // PAD 6
    0x26: -8, // PAD 7
    0x27: 8   // PAD 8
};

PioneerDDJFLX2.quickJumpSize = 32;

// Used for tempo slider
PioneerDDJFLX2.highResMSB = {
    "[Channel1]": {},
    "[Channel2]": {}
};

PioneerDDJFLX2.trackLoadedLED = function(value, group, _control) {
    midi.sendShortMsg(
        0x9F,
        group.match(script.channelRegEx)[1] - 1,
        value > 0 ? 0x7F : 0x00
    );
};

PioneerDDJFLX2.toggleLight = function(midiIn, active) {
    midi.sendShortMsg(midiIn.status, midiIn.data1, active ? 0x7F : 0);
};

//
// Init
//

PioneerDDJFLX2.init = function() {
    engine.setValue("[EffectRack1_EffectUnit1]", "show_focus", 1);

    engine.makeUnbufferedConnection("[Channel1]", "vu_meter", PioneerDDJFLX2.vuMeterUpdate);
    engine.makeUnbufferedConnection("[Channel2]", "vu_meter", PioneerDDJFLX2.vuMeterUpdate);

    PioneerDDJFLX2.toggleLight(PioneerDDJFLX2.lights.deck1.vuMeter, false);
    PioneerDDJFLX2.toggleLight(PioneerDDJFLX2.lights.deck2.vuMeter, false);

    engine.softTakeover("[Channel1]", "rate", true);
    engine.softTakeover("[Channel2]", "rate", true);
    engine.softTakeover("[EffectRack1_EffectUnit1_Effect1]", "meta", true);
    engine.softTakeover("[EffectRack1_EffectUnit1_Effect2]", "meta", true);
    engine.softTakeover("[EffectRack1_EffectUnit1_Effect3]", "meta", true);
    engine.softTakeover("[EffectRack1_EffectUnit1]", "mix", true);

    const samplerCount = 16;
    if (engine.getValue("[App]", "num_samplers") < samplerCount) {
        engine.setValue("[App]", "num_samplers", samplerCount);
    }
    for (let i = 1; i <= samplerCount; ++i) {
        engine.makeConnection("[Sampler" + i + "]", "play", PioneerDDJFLX2.samplerPlayOutputCallbackFunction);
    }

    engine.makeConnection("[Channel1]", "track_loaded", PioneerDDJFLX2.trackLoadedLED);
    engine.makeConnection("[Channel2]", "track_loaded", PioneerDDJFLX2.trackLoadedLED);

    // play the "track loaded" animation on both decks at startup
    midi.sendShortMsg(0x9F, 0x00, 0x7F);
    midi.sendShortMsg(0x9F, 0x01, 0x7F);

    PioneerDDJFLX2.setLoopButtonLights(0x90, 0x7F);
    PioneerDDJFLX2.setLoopButtonLights(0x91, 0x7F);

    engine.makeConnection("[Channel1]", "loop_enabled", PioneerDDJFLX2.loopToggle);
    engine.makeConnection("[Channel2]", "loop_enabled", PioneerDDJFLX2.loopToggle);

    engine.makeConnection("[Channel1]", "loop_start_position", PioneerDDJFLX2.loopInPending);
    engine.makeConnection("[Channel2]", "loop_start_position", PioneerDDJFLX2.loopInPending);
    engine.makeConnection("[Channel1]", "loop_end_position", PioneerDDJFLX2.loopInPending);
    engine.makeConnection("[Channel2]", "loop_end_position", PioneerDDJFLX2.loopInPending);

    for (let i = 1; i <= 3; i++) {
        engine.makeConnection("[EffectRack1_EffectUnit1_Effect" + i +"]", "enabled", PioneerDDJFLX2.toggleFxLight);
    }
    engine.makeConnection("[EffectRack1_EffectUnit1]", "focused_effect", PioneerDDJFLX2.toggleFxLight);

    // Bite DJ: jog mode is chosen in the in-skin General settings tab via the
    // [BiteDJ],vinyl_mode CO (1 = Vinyl/scratch, 0 = CDJ/pitch-bend). Subscribe
    // so the choice applies live, and trigger() once to seed the current value.
    // On an unpatched Mixxx the CO does not exist, makeConnection returns nothing,
    // and we keep the hard-coded default above.
    const vinylModeConnection = engine.makeConnection(
        "[BiteDJ]", "vinyl_mode", PioneerDDJFLX2.setVinylMode);
    if (vinylModeConnection) {
        vinylModeConnection.trigger();
    }

    // query the controller for current control positions on startup
    midi.sendSysexMsg([0xF0, 0x00, 0x40, 0x05, 0x00, 0x00, 0x02, 0x06, 0x00, 0x03, 0x01, 0xf7], 12);
};

//
// Waveform zoom
//

PioneerDDJFLX2.waveformZoom = function(midichan, control, value, status, group) {
    if (value === 0x7f) {
        script.triggerControl(group, "waveform_zoom_up", 100);
    } else {
        script.triggerControl(group, "waveform_zoom_down", 100);
    }
};

// BROWSE rotate: zoom the waveform while the play screen ([Tab],current == 0)
// is active, otherwise scroll the library as usual.
PioneerDDJFLX2.browseRotate = function(midichan, control, value, status) {
    if (engine.getValue("[Tab]", "current") === 0) {
        PioneerDDJFLX2.waveformZoom(midichan, control, value, status, "[Channel1]");
    } else {
        engine.setValue("[Library]", "MoveVertical", value > 0x40 ? value - 0x80 : value);
    }
};

// From Play, BROWSE press opens the browser. On the browser and other screens
// it retains the mapping's existing focus-forward behavior.
PioneerDDJFLX2.browsePress = function(_midichan, _control, value) {
    if (value === 0) {
        return;
    }
    if (engine.getValue("[Tab]", "current") === 0) {
        engine.setValue("[Tab]", "current", 1);
        engine.setValue("[Tab]", "library", 1);
        return;
    }
    script.triggerControl("[Library]", "MoveFocusForward", 100);
};

// SHIFT + right LOAD toggles the Browser.
PioneerDDJFLX2.toggleBrowser = function(_midichan, _control, value) {
    if (value === 0) {
        return;
    }
    const browserOpen = engine.getValue("[Tab]", "current") === 1;
    engine.setValue("[Tab]", "current", browserOpen ? 0 : 1);
    engine.setValue("[Tab]", browserOpen ? "overview" : "library", 1);
};

//
// Channel level lights
//

PioneerDDJFLX2.vuMeterUpdate = function(value, group) {
    const newVal = value * 150;

    switch (group) {
    case "[Channel1]":
        midi.sendShortMsg(0xB0, 0x02, newVal);
        break;

    case "[Channel2]":
        midi.sendShortMsg(0xB1, 0x02, newVal);
        break;
    }
};

//
// Effects
//

PioneerDDJFLX2.toggleFxLight = function(_value, _group, _control) {
    const enabled = engine.getValue(PioneerDDJFLX2.focusedFxGroup(), "enabled");

    PioneerDDJFLX2.toggleLight(PioneerDDJFLX2.lights.beatFx, enabled);
    PioneerDDJFLX2.toggleLight(PioneerDDJFLX2.lights.shiftBeatFx, enabled);
};

PioneerDDJFLX2.focusedFxGroup = function() {
    const focusedFx = engine.getValue("[EffectRack1_EffectUnit1]", "focused_effect");
    return "[EffectRack1_EffectUnit1_Effect" + focusedFx + "]";
};

PioneerDDJFLX2.beatFxLevelDepthRotate = function(_channel, _control, value) {
    if (PioneerDDJFLX2.shiftButtonDown[0] || PioneerDDJFLX2.shiftButtonDown[1]) {
        engine.softTakeoverIgnoreNextValue("[EffectRack1_EffectUnit1]", "mix");
        engine.setParameter(PioneerDDJFLX2.focusedFxGroup(), "meta", value / 0x7F);
    } else {
        engine.softTakeoverIgnoreNextValue(PioneerDDJFLX2.focusedFxGroup(), "meta");
        engine.setParameter("[EffectRack1_EffectUnit1]", "mix", value / 0x7F);
    }
};

// Bite DJ skin only renders one effect slot (Effect1), so the BEAT
// LEFT/RIGHT buttons step through the on-screen bucket grid of the
// loaded Beats-typed parameter instead of switching focused slot.
// Order matches the row template's reading order
// (⅛ → ¼ → ½ → 1 → 2 → 4), values are raw rate-in-cycles-per-beat.
PioneerDDJFLX2.beatFxBuckets = [8, 4, 2, 1, 0.5, 0.25];

PioneerDDJFLX2.findBeatsParameter = function(group) {
    for (let i = 1; i <= 16; i++) {
        if (engine.getValue(group, "parameter" + i + "_loaded") !== 1) {
            continue;
        }
        if (engine.getValue(group, "parameter" + i + "_units") === 1) {
            return i;
        }
    }
    return -1;
};

PioneerDDJFLX2.stepBeatFxBucket = function(direction) {
    const group = "[EffectRack1_EffectUnit1_Effect1]";
    const paramIndex = PioneerDDJFLX2.findBeatsParameter(group);
    if (paramIndex === -1) { return; }

    const buckets = PioneerDDJFLX2.beatFxBuckets;
    const valueKey = "parameter" + paramIndex + "_value";
    const current = engine.getValue(group, valueKey);

    // Snap to nearest bucket, then step. Off-bucket values (rare —
    // bucket presses are the only writes — but possible via a MIDI
    // mapping that pokes a raw value) round to the closest match.
    let closest = 0;
    let bestDist = Math.abs(buckets[0] - current);
    for (let i = 1; i < buckets.length; i++) {
        const dist = Math.abs(buckets[i] - current);
        if (dist < bestDist) {
            bestDist = dist;
            closest = i;
        }
    }

    let next = closest + direction;
    if (next < 0) { next = 0; }
    if (next >= buckets.length) { next = buckets.length - 1; }
    if (next === closest) { return; }

    engine.setValue(group, valueKey, buckets[next]);
};

PioneerDDJFLX2.beatFxLeftPressed = function(_channel, _control, value) {
    if (value === 0) { return; }

    PioneerDDJFLX2.stepBeatFxBucket(-1);
};

PioneerDDJFLX2.beatFxRightPressed = function(_channel, _control, value) {
    if (value === 0) { return; }

    PioneerDDJFLX2.stepBeatFxBucket(1);
};

PioneerDDJFLX2.beatFxSelectPressed = function(_channel, _control, value) {
    if (value === 0) { return; }

    engine.setValue(PioneerDDJFLX2.focusedFxGroup(), "next_effect", value);
};

PioneerDDJFLX2.beatFxSelectShiftPressed = function(_channel, _control, value) {
    if (value === 0) { return; }

    engine.setValue(PioneerDDJFLX2.focusedFxGroup(), "prev_effect", value);
};

PioneerDDJFLX2.beatFxOnOffPressed = function(_channel, _control, value) {
    if (value === 0) { return; }

    const toggleEnabled = !engine.getValue(PioneerDDJFLX2.focusedFxGroup(), "enabled");
    engine.setValue(PioneerDDJFLX2.focusedFxGroup(), "enabled", toggleEnabled);
};

PioneerDDJFLX2.beatFxOnOffShiftPressed = function(_channel, _control, value) {
    if (value === 0) { return; }

    engine.setParameter("[EffectRack1_EffectUnit1]", "mix", 0);
    engine.softTakeoverIgnoreNextValue("[EffectRack1_EffectUnit1]", "mix");

    for (let i = 1; i <= 3; i++) {
        engine.setValue("[EffectRack1_EffectUnit1_Effect" + i + "]", "enabled", 0);
    }
    PioneerDDJFLX2.toggleLight(PioneerDDJFLX2.lights.beatFx, false);
    PioneerDDJFLX2.toggleLight(PioneerDDJFLX2.lights.shiftBeatFx, false);
};

PioneerDDJFLX2.beatFxChannel = function(_channel, control, value, _status, group) {
    if (value === 0x00) { return; }

    // MASTER routes the FX to both decks so that both DECK ASSIGN buttons in the
    // FX pane stay highlighted. We enable the individual channels rather than
    // group_[Master]_enable so the on-screen buttons (bound to the per-channel
    // enables) reflect the selection.
    const master = control === 0x14,
        enableChannel1 = control === 0x10 || master ? 1 : 0,
        enableChannel2 = control === 0x11 || master ? 1 : 0;

    engine.setValue(group, "group_[Channel1]_enable", enableChannel1);
    engine.setValue(group, "group_[Channel2]_enable", enableChannel2);
    engine.setValue(group, "group_[Master]_enable", 0);
};

//
// Loop IN/OUT ADJUST
//

PioneerDDJFLX2.toggleLoopAdjustIn = function(channel, _control, value, _status, group) {
    if (value === 0 || engine.getValue(group, "loop_enabled" === 0)) {
        return;
    }
    PioneerDDJFLX2.loopAdjustIn[channel] = !PioneerDDJFLX2.loopAdjustIn[channel];
    PioneerDDJFLX2.loopAdjustOut[channel] = false;
};

PioneerDDJFLX2.toggleLoopAdjustOut = function(channel, _control, value, _status, group) {
    if (value === 0 || engine.getValue(group, "loop_enabled" === 0)) {
        return;
    }
    PioneerDDJFLX2.loopAdjustOut[channel] = !PioneerDDJFLX2.loopAdjustOut[channel];
    PioneerDDJFLX2.loopAdjustIn[channel] = false;
};

// Two signals are sent here so that the light stays lit/unlit in its shift state too
PioneerDDJFLX2.setReloopLight = function(status, value) {
    midi.sendShortMsg(status, 0x4D, value);
    midi.sendShortMsg(status, 0x50, value);
};


PioneerDDJFLX2.setLoopButtonLights = function(status, value) {
    [0x10, 0x11, 0x4E, 0x4C].forEach(function(control) {
        midi.sendShortMsg(status, control, value);
    });
};

PioneerDDJFLX2.startLoopLightsBlink = function(channel, control, status, group) {
    let blink = 0x7F;

    PioneerDDJFLX2.stopLoopLightsBlink(group, control, status);

    PioneerDDJFLX2.timers[group][control] = engine.beginTimer(500, () => {
        blink = 0x7F - blink;

        // When adjusting the loop out position, turn the loop in light off
        if (PioneerDDJFLX2.loopAdjustOut[channel]) {
            midi.sendShortMsg(status, 0x10, 0x00);
            midi.sendShortMsg(status, 0x4C, 0x00);
        } else {
            midi.sendShortMsg(status, 0x10, blink);
            midi.sendShortMsg(status, 0x4C, blink);
        }

        // When adjusting the loop in position, turn the loop out light off
        if (PioneerDDJFLX2.loopAdjustIn[channel]) {
            midi.sendShortMsg(status, 0x11, 0x00);
            midi.sendShortMsg(status, 0x4E, 0x00);
        } else {
            midi.sendShortMsg(status, 0x11, blink);
            midi.sendShortMsg(status, 0x4E, blink);
        }
    });

};

PioneerDDJFLX2.stopLoopLightsBlink = function(group, control, status) {
    PioneerDDJFLX2.timers[group] = PioneerDDJFLX2.timers[group] || {};

    if (PioneerDDJFLX2.timers[group][control] !== undefined) {
        engine.stopTimer(PioneerDDJFLX2.timers[group][control]);
    }
    PioneerDDJFLX2.timers[group][control] = undefined;
    PioneerDDJFLX2.setLoopButtonLights(status, 0x7F);
};

PioneerDDJFLX2.loopToggle = function(value, group, control) {
    const status = group === "[Channel1]" ? 0x90 : 0x91,
        channel = group === "[Channel1]" ? 0 : 1;

    PioneerDDJFLX2.setReloopLight(status, value ? 0x7F : 0x00);

    if (value) {
        PioneerDDJFLX2.stopLoopInPendingBlink(status, group);
        PioneerDDJFLX2.startLoopLightsBlink(channel, control, status, group);
    } else {
        PioneerDDJFLX2.stopLoopLightsBlink(group, control, status);
        PioneerDDJFLX2.loopAdjustIn[channel] = false;
        PioneerDDJFLX2.loopAdjustOut[channel] = false;
    }
};

// loop_enabled stays 0 until OUT is pressed, so we watch the position COs.
PioneerDDJFLX2.loopInPending = function(_value, group) {
    const status = group === "[Channel1]" ? 0x90 : 0x91;

    if (engine.getValue(group, "loop_enabled") > 0) {
        return;
    }

    const inSet = engine.getValue(group, "loop_start_position") >= 0;
    const outSet = engine.getValue(group, "loop_end_position") >= 0;

    if (inSet && !outSet) {
        PioneerDDJFLX2.startLoopInPendingBlink(status, group);
    } else {
        PioneerDDJFLX2.stopLoopInPendingBlink(status, group);
    }
};

PioneerDDJFLX2.startLoopInPendingBlink = function(status, group) {
    PioneerDDJFLX2.stopLoopInPendingBlink(status, group);

    let blink = 0x7F;
    PioneerDDJFLX2.timers[group] = PioneerDDJFLX2.timers[group] || {};
    PioneerDDJFLX2.timers[group]["loopInPending"] = engine.beginTimer(500, () => {
        blink = 0x7F - blink;
        midi.sendShortMsg(status, 0x10, blink);
        midi.sendShortMsg(status, 0x4C, blink);
    });
};

PioneerDDJFLX2.stopLoopInPendingBlink = function(status, group) {
    PioneerDDJFLX2.timers[group] = PioneerDDJFLX2.timers[group] || {};
    if (PioneerDDJFLX2.timers[group]["loopInPending"] !== undefined) {
        engine.stopTimer(PioneerDDJFLX2.timers[group]["loopInPending"]);
        PioneerDDJFLX2.timers[group]["loopInPending"] = undefined;
        midi.sendShortMsg(status, 0x10, 0x7F);
        midi.sendShortMsg(status, 0x4C, 0x7F);
    }
};

//
// CUE/LOOP CALL
//

PioneerDDJFLX2.cueLoopCallLeft = function(_channel, _control, value, _status, group) {
    if (value) {
        if (engine.getValue(group, "loop_enabled") > 0) {
            engine.setValue(group, "loop_scale", 0.5);
        } else {
            engine.setValue(group, "memorycue_prev", 1);
        }
    }
};

PioneerDDJFLX2.cueLoopCallRight = function(_channel, _control, value, _status, group) {
    if (value) {
        if (engine.getValue(group, "loop_enabled") > 0) {
            engine.setValue(group, "loop_scale", 2.0);
        } else {
            engine.setValue(group, "memorycue_next", 1);
        }
    }
};

PioneerDDJFLX2.memoryCueSet = function(_channel, _control, value, _status, group) {
    if (value) {
        engine.setValue(group, "memorycue_set", 1);
    }
};

PioneerDDJFLX2.memoryCueDelete = function(_channel, _control, value, _status, group) {
    if (value) {
        engine.setValue(group, "memorycue_delete", 1);
    }
};

//
// BEAT SYNC
//
// Note that the controller sends different signals for a short press and a long
// press of the same button.
//

PioneerDDJFLX2.syncPressed = function(channel, control, value, status, group) {
    if (engine.getValue(group, "sync_enabled") && value > 0) {
        engine.setValue(group, "sync_enabled", 0);
    } else {
        engine.setValue(group, "beatsync", value);
    }
};

PioneerDDJFLX2.syncLongPressed = function(channel, control, value, status, group) {
    if (value) {
        engine.setValue(group, "sync_enabled", 1);
    }
};

PioneerDDJFLX2.cycleTempoRange = function(_channel, _control, value, _status, group) {
    if (value === 0) { return; } // ignore release

    const currRange = engine.getValue(group, "rateRange");
    let idx = 0;

    for (let i = 0; i < this.tempoRanges.length; i++) {
        if (currRange === this.tempoRanges[i]) {
            idx = (i + 1) % this.tempoRanges.length;
            break;
        }
    }
    engine.setValue(group, "rateRange", this.tempoRanges[idx]);
};

//
// Jog wheels
//

PioneerDDJFLX2.jogTurn = function(channel, _control, value, _status, group) {
    const deckNum = channel + 1;
    // wheel center at 64; <64 rew >64 fwd
    let newVal = value - 64;

    // loop_in / out adjust
    const loopEnabled = engine.getValue(group, "loop_enabled");
    if (loopEnabled > 0) {
        if (PioneerDDJFLX2.loopAdjustIn[channel]) {
            newVal = newVal * PioneerDDJFLX2.loopAdjustMultiply + engine.getValue(group, "loop_start_position");
            engine.setValue(group, "loop_start_position", newVal);
            return;
        }
        if (PioneerDDJFLX2.loopAdjustOut[channel]) {
            newVal = newVal * PioneerDDJFLX2.loopAdjustMultiply + engine.getValue(group, "loop_end_position");
            engine.setValue(group, "loop_end_position", newVal);
            return;
        }
    }

    if (engine.isScratching(deckNum)) {
        engine.scratchTick(deckNum, newVal);
    } else { // fallback
        engine.setValue(group, "jog", newVal * this.bendScale);
    }
};


PioneerDDJFLX2.jogSearch = function(_channel, _control, value, _status, group) {
    const newVal = (value - 64) * PioneerDDJFLX2.fastSeekScale;
    engine.setValue(group, "jog", newVal);
};

// Connection callback for [BiteDJ],vinyl_mode. Maps the CO (1 = Vinyl,
// 0 = CDJ) onto the boolean jogTouch() checks before enabling scratching.
PioneerDDJFLX2.setVinylMode = function(value) {
    PioneerDDJFLX2.vinylMode = value !== 0;
};

PioneerDDJFLX2.jogTouch = function(channel, _control, value) {
    const deckNum = channel + 1;

    // skip while adjusting the loop points
    if (PioneerDDJFLX2.loopAdjustIn[channel] || PioneerDDJFLX2.loopAdjustOut[channel]) {
        return;
    }

    if (value !== 0 && this.vinylMode) {
        engine.scratchEnable(deckNum, 720, 33+1/3, this.alpha, this.beta);
    } else {
        engine.scratchDisable(deckNum);
    }
};

//
// Shift button
//

PioneerDDJFLX2.shiftPressed = function(channel, _control, value, _status, _group) {
    PioneerDDJFLX2.shiftButtonDown[channel] = value === 0x7F;
};


//
// Tempo sliders
//
// The tempo option in Mixxx's deck preferences determine whether down/up
// increases/decreases the rate. Therefore it must be inverted here so that the
// UI and the control sliders always move in the same direction.
//

PioneerDDJFLX2.tempoSliderMSB = function(channel, control, value, status, group) {
    PioneerDDJFLX2.highResMSB[group].tempoSlider = value;
};

PioneerDDJFLX2.tempoSliderLSB = function(channel, control, value, status, group) {
    const fullValue = (PioneerDDJFLX2.highResMSB[group].tempoSlider << 7) + value;

    engine.setValue(
        group,
        "rate",
        1 - (fullValue / 0x2000)
    );
};

//
// Beat Jump mode
//
// Note that when we increase/decrease the sizes on the pad buttons, we use the
// value of the first pad (0x21) as an upper/lower limit beyond which we don't
// allow further increasing/decreasing of all the values.
//

PioneerDDJFLX2.beatjumpPadPressed = function(_channel, control, value, _status, group) {
    if (value === 0) {
        return;
    }
    engine.setValue(group, "beatjump_size", Math.abs(PioneerDDJFLX2.beatjumpSizeForPad[control]));
    engine.setValue(group, "beatjump", PioneerDDJFLX2.beatjumpSizeForPad[control]);
};

PioneerDDJFLX2.increaseBeatjumpSizes = function(_channel, control, value, _status, group) {
    if (value === 0 || PioneerDDJFLX2.beatjumpSizeForPad[0x21] * 16 > 16) {
        return;
    }
    Object.keys(PioneerDDJFLX2.beatjumpSizeForPad).forEach(function(pad) {
        PioneerDDJFLX2.beatjumpSizeForPad[pad] = PioneerDDJFLX2.beatjumpSizeForPad[pad] * 16;
    });
    engine.setValue(group, "beatjump_size", PioneerDDJFLX2.beatjumpSizeForPad[0x21]);
};

PioneerDDJFLX2.decreaseBeatjumpSizes = function(_channel, control, value, _status, group) {
    if (value === 0 || PioneerDDJFLX2.beatjumpSizeForPad[0x21] / 16 < 1/16) {
        return;
    }
    Object.keys(PioneerDDJFLX2.beatjumpSizeForPad).forEach(function(pad) {
        PioneerDDJFLX2.beatjumpSizeForPad[pad] = PioneerDDJFLX2.beatjumpSizeForPad[pad] / 16;
    });
    engine.setValue(group, "beatjump_size", PioneerDDJFLX2.beatjumpSizeForPad[0x21]);
};

//
// Sampler mode
//

PioneerDDJFLX2.samplerPlayOutputCallbackFunction = function(value, group, _control) {
    if (value === 1) {
        const curPad = group.match(script.samplerRegEx)[1];
        PioneerDDJFLX2.startSamplerBlink(
            0x97 + (curPad > 8 ? 2 : 0),
            0x30 + ((curPad > 8 ? curPad - 8 : curPad) - 1),
            group);
    }
};

PioneerDDJFLX2.samplerPadPressed = function(_channel, _control, value, _status, group) {
    if (engine.getValue(group, "track_loaded")) {
        engine.setValue(group, "cue_gotoandplay", value);
    } else {
        engine.setValue(group, "LoadSelectedTrack", value);
    }
};

PioneerDDJFLX2.samplerPadShiftPressed = function(_channel, _control, value, _status, group) {
    if (engine.getValue(group, "play")) {
        engine.setValue(group, "cue_gotoandstop", value);
    } else if (engine.getValue(group, "track_loaded")) {
        engine.setValue(group, "eject", value);
    }
};

PioneerDDJFLX2.startSamplerBlink = function(channel, control, group) {
    let val = 0x7f;

    PioneerDDJFLX2.stopSamplerBlink(channel, control);
    PioneerDDJFLX2.timers[channel][control] = engine.beginTimer(250, () => {
        val = 0x7f - val;

        // blink the appropriate pad
        midi.sendShortMsg(channel, control, val);
        // also blink the pad while SHIFT is pressed
        midi.sendShortMsg((channel+1), control, val);

        const isPlaying = engine.getValue(group, "play") === 1;

        if (!isPlaying) {
            // kill timer
            PioneerDDJFLX2.stopSamplerBlink(channel, control);
            // set the pad LED to ON
            midi.sendShortMsg(channel, control, 0x7f);
            // set the pad LED to ON while SHIFT is pressed
            midi.sendShortMsg((channel+1), control, 0x7f);
        }
    });
};

PioneerDDJFLX2.stopSamplerBlink = function(channel, control) {
    PioneerDDJFLX2.timers[channel] = PioneerDDJFLX2.timers[channel] || {};

    if (PioneerDDJFLX2.timers[channel][control] !== undefined) {
        engine.stopTimer(PioneerDDJFLX2.timers[channel][control]);
        PioneerDDJFLX2.timers[channel][control] = undefined;
    }
};

//
// Additional features
//

PioneerDDJFLX2.toggleQuantize = function(_channel, _control, value, _status, group) {
    if (value) {
        script.toggleControl(group, "quantize");
    }
};

PioneerDDJFLX2.quickJumpForward = function(_channel, _control, value, _status, group) {
    if (value) {
        engine.setValue(group, "beatjump", PioneerDDJFLX2.quickJumpSize);
    }
};

PioneerDDJFLX2.quickJumpBack = function(_channel, _control, value, _status, group) {
    if (value) {
        engine.setValue(group, "beatjump", -PioneerDDJFLX2.quickJumpSize);
    }
};

//
// Shutdown
//

PioneerDDJFLX2.shutdown = function() {
    // reset vumeter
    PioneerDDJFLX2.toggleLight(PioneerDDJFLX2.lights.deck1.vuMeter, false);
    PioneerDDJFLX2.toggleLight(PioneerDDJFLX2.lights.deck2.vuMeter, false);

    // housekeeping
    // turn off all Sampler LEDs
    for (let i = 0; i <= 7; ++i) {
        midi.sendShortMsg(0x97, 0x30 + i, 0x00);    // Deck 1 pads
        midi.sendShortMsg(0x98, 0x30 + i, 0x00);    // Deck 1 pads with SHIFT
        midi.sendShortMsg(0x99, 0x30 + i, 0x00);    // Deck 2 pads
        midi.sendShortMsg(0x9A, 0x30 + i, 0x00);    // Deck 2 pads with SHIFT
    }
    // turn off all Hotcue LEDs
    for (let i = 0; i <= 7; ++i) {
        midi.sendShortMsg(0x97, 0x00 + i, 0x00);    // Deck 1 pads
        midi.sendShortMsg(0x98, 0x00 + i, 0x00);    // Deck 1 pads with SHIFT
        midi.sendShortMsg(0x99, 0x00 + i, 0x00);    // Deck 2 pads
        midi.sendShortMsg(0x9A, 0x00 + i, 0x00);    // Deck 2 pads with SHIFT
    }

    // turn off loop in and out lights
    PioneerDDJFLX2.setLoopButtonLights(0x90, 0x00);
    PioneerDDJFLX2.setLoopButtonLights(0x91, 0x00);

    // turn off reloop lights
    PioneerDDJFLX2.setReloopLight(0x90, 0x00);
    PioneerDDJFLX2.setReloopLight(0x91, 0x00);

    // stop any flashing lights
    PioneerDDJFLX2.toggleLight(PioneerDDJFLX2.lights.beatFx, false);
    PioneerDDJFLX2.toggleLight(PioneerDDJFLX2.lights.shiftBeatFx, false);
};
