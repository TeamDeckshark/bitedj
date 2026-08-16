// =============================================================================
// Rally Audio Takeout Box -- Mixxx Controller Script (2-Deck + Crossfader)
//
// Companion file: Rally-Audio-Takeout-Box.midi.xml
// Target: Mixxx 2.4+
// USB endpoint: "Takeout MIDI+CDC"
//
// Architecture: MIDI channels 1-2 map to Mixxx decks 1-2.
// All handler functions derive the deck from the MIDI channel parameter.
// =============================================================================

// eslint-disable-next-line no-var
var TakeoutBox = {};

// -----------------------------------------------------------------------------
// SCRATCH / JOG TUNING
// -----------------------------------------------------------------------------
// intervalsPerRev: virtual "ticks per revolution" of the imaginary record.
//   Higher = finer resolution per touch movement.
//   Typical jog wheel: 128. Touch strip (Trill Bar): try 512-2048.
TakeoutBox.scratchIntervalsPerRev = 512;

// scratchRPM: speed of the imaginary record at 0% pitch.
TakeoutBox.scratchRPM = 33 + 1/3;

// alpha: responsiveness of the scratch filter while finger is down (0-1).
//   Higher = snappier tracking. Lower = smoother but laggier.
TakeoutBox.scratchAlpha = 0.5;

// beta: velocity prediction while finger is down.
//   Keep low to avoid the rubber-band bounce on release.
TakeoutBox.scratchBeta = 0.5 / 32;

// --- INERTIA (coast after finger release) ---
// inertiaFriction: multiplied into velocity each timer tick (0-1).
//   Lower = faster deceleration. 0.9 = long coast, 0.5 = quick stop.
TakeoutBox.inertiaFriction = 0.85;

// inertiaTimerMs: how often the coast timer fires (milliseconds).
TakeoutBox.inertiaTimerMs = 20;

// inertiaMinVelocity: stop coasting when velocity drops below this.
TakeoutBox.inertiaMinVelocity = 0.2;

// velocitySmoothing: exponential moving average factor for velocity tracking
//   while finger is down (0-1). Higher = more weight on latest tick.
TakeoutBox.velocitySmoothing = 0.4;

// jogSensitivity: multiplier for jog pitch-bend when NOT scratching.
TakeoutBox.jogSensitivity = 1.0;

// Per-deck state for the inertia system
TakeoutBox.deckState = {};
TakeoutBox.initDeckState = function(deck) {
    TakeoutBox.deckState[deck] = {
        velocity: 0,
        inertiaTimer: 0,
    };
};

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------

TakeoutBox.deckFromChannel = function(channel) {
    return (channel & 0x0F) + 1;
};

TakeoutBox.groupFromChannel = function(channel) {
    return "[Channel" + TakeoutBox.deckFromChannel(channel) + "]";
};

// -----------------------------------------------------------------------------
// Init / Shutdown
// -----------------------------------------------------------------------------
TakeoutBox.init = function(id, debugging) {
    console.log("Takeout Box (2-deck): initializing (id=" + id + ")");

    for (var d = 1; d <= 2; d++) {
        var g = "[Channel" + d + "]";

        // Soft-takeover on absolute knobs
        engine.softTakeover("[QuickEffectRack1_" + g + "]", "super1", true);
        engine.softTakeover("[EqualizerRack1_" + g + "_Effect1]", "parameter1", true);
        engine.softTakeover("[EqualizerRack1_" + g + "_Effect1]", "parameter2", true);
        engine.softTakeover("[EqualizerRack1_" + g + "_Effect1]", "parameter3", true);

        TakeoutBox.initDeckState(d);
    }

    engine.softTakeover("[Master]", "crossfader", true);
};

TakeoutBox.shutdown = function() {
    for (var d = 1; d <= 2; d++) {
        var state = TakeoutBox.deckState[d];
        if (state && state.inertiaTimer !== 0) {
            engine.stopTimer(state.inertiaTimer);
        }
    }
    console.log("Takeout Box (2-deck): shutting down");
};


// =============================================================================
//  TRANSPORT
// =============================================================================

// Play / Pause toggle (note 0)
TakeoutBox.playPause = function(channel, _control, value, _status, _group) {
    if (value === 0x7F) {
        script.toggleControl(TakeoutBox.groupFromChannel(channel), "play");
    }
};


// =============================================================================
//  CROSSFADER (CC 75, absolute, flipped)
//  Both decks send the same CC on their respective channels -- either one
//  moves the same master crossfader.
// =============================================================================

TakeoutBox.crossfader = function(_channel, _control, value, _status, _group) {
    // Flipped: 0 maps to +1 (right), 127 maps to -1 (left)
    engine.setParameter("[Master]", "crossfader", 1.0 - (value / 127));
};


// =============================================================================
//  SCRATCHING (touch sensor + wheel + inertia coast)
// =============================================================================

// Scratch touch on/off (note 91)
TakeoutBox.wheelTouch = function(channel, _control, value, _status, _group) {
    var deck  = TakeoutBox.deckFromChannel(channel);
    var state = TakeoutBox.deckState[deck];

    if (value === 0x7F) {
        // Finger down: stop any running inertia coast
        if (state.inertiaTimer !== 0) {
            engine.stopTimer(state.inertiaTimer);
            state.inertiaTimer = 0;
        }
        state.velocity = 0;

        engine.scratchEnable(deck,
            TakeoutBox.scratchIntervalsPerRev,
            TakeoutBox.scratchRPM,
            TakeoutBox.scratchAlpha,
            TakeoutBox.scratchBeta,
            false);
    } else {
        // Finger up: begin inertia coast
        TakeoutBox.startInertia(deck);
    }
};

// Scratch wheel movement (CC 33, relative centered on 64)
TakeoutBox.scratchMove = function(channel, _control, value, _status, _group) {
    var deck  = TakeoutBox.deckFromChannel(channel);
    var delta = value - 64;

    if (engine.isScratching(deck)) {
        var state = TakeoutBox.deckState[deck];
        var s = TakeoutBox.velocitySmoothing;
        state.velocity = (s * delta) + ((1 - s) * state.velocity);
        engine.scratchTick(deck, delta);
    } else {
        engine.setValue(TakeoutBox.groupFromChannel(channel),
                        "jog", delta * TakeoutBox.jogSensitivity);
    }
};

// Inertia coast engine
TakeoutBox.startInertia = function(deck) {
    var state = TakeoutBox.deckState[deck];
    var vel   = state.velocity;

    if (Math.abs(vel) < TakeoutBox.inertiaMinVelocity) {
        engine.scratchDisable(deck, false);
        return;
    }

    state.inertiaTimer = engine.beginTimer(TakeoutBox.inertiaTimerMs, function() {
        vel *= TakeoutBox.inertiaFriction;

        if (Math.abs(vel) < TakeoutBox.inertiaMinVelocity) {
            engine.stopTimer(state.inertiaTimer);
            state.inertiaTimer = 0;
            engine.scratchDisable(deck, false);
            return;
        }

        var tick = (vel > 0) ? Math.ceil(vel) : Math.floor(vel);
        if (tick !== 0) {
            engine.scratchTick(deck, tick);
        }
    });
};


// =============================================================================
//  SPEED / RATE ENCODER (CC 36, relative centered on 64, flipped)
// =============================================================================

TakeoutBox.speedKnob = function(channel, _control, value, _status, _group) {
    var g        = TakeoutBox.groupFromChannel(channel);
    var delta    = -(value - 64);   // flipped per djay mapping
    var current  = engine.getParameter(g, "rate");
    var stepSize = 0.005;
    var newVal   = Math.max(0, Math.min(1, current + (delta * stepSize)));
    engine.setParameter(g, "rate", newVal);
};


// =============================================================================
//  FILTER / EQ (absolute CC 0-127)
// =============================================================================

TakeoutBox.filter = function(channel, _control, value, _status, _group) {
    var g = TakeoutBox.groupFromChannel(channel);
    engine.setParameter("[QuickEffectRack1_" + g + "]", "super1", value / 127);
};

TakeoutBox.eqHigh = function(channel, _control, value, _status, _group) {
    var g = TakeoutBox.groupFromChannel(channel);
    engine.setParameter("[EqualizerRack1_" + g + "_Effect1]", "parameter3", value / 127);
};

TakeoutBox.eqMid = function(channel, _control, value, _status, _group) {
    var g = TakeoutBox.groupFromChannel(channel);
    engine.setParameter("[EqualizerRack1_" + g + "_Effect1]", "parameter2", value / 127);
};

TakeoutBox.eqLow = function(channel, _control, value, _status, _group) {
    var g = TakeoutBox.groupFromChannel(channel);
    engine.setParameter("[EqualizerRack1_" + g + "_Effect1]", "parameter1", value / 127);
};


// =============================================================================
//  MATCH KEY (note 99)
//  djay: matchKey -- match this deck's key to the other deck.
//  Mixxx: sync_key does a one-shot key sync to the other playing deck.
// =============================================================================

TakeoutBox.matchKey = function(channel, _control, value, _status, _group) {
    if (value === 0x7F) {
        engine.setValue(TakeoutBox.groupFromChannel(channel), "sync_key", 1);
    }
};


// =============================================================================
//  SAMPLERS (notes 54,56,58,60,62,64,66,68 -> Sampler 1-8)
// =============================================================================

TakeoutBox.samplerNotes = {
    54: 1, 56: 2, 58: 3, 60: 4,
    62: 5, 64: 6, 66: 7, 68: 8
};

TakeoutBox.samplerButton = function(_channel, control, value, _status, _group) {
    if (value !== 0x7F) { return; }

    var num = TakeoutBox.samplerNotes[control];
    if (num === undefined) { return; }

    var g = "[Sampler" + num + "]";
    if (engine.getValue(g, "play") > 0) {
        engine.setValue(g, "play", 0);
    } else {
        engine.setValue(g, "cue_gotoandplay", 1);
    }
};


// =============================================================================
//  LIBRARY CONTROLS
// =============================================================================

// Library scroll encoder (CC 50, relative centered on 64, flipped)
TakeoutBox.libraryScroll = function(_channel, _control, value, _status, _group) {
    var delta = -(value - 64);   // flipped
    engine.setValue("[Library]", "MoveVertical", delta);
};

// Library back (note 83)
TakeoutBox.libraryBack = function(_channel, _control, value, _status, _group) {
    if (value === 0x7F) {
        engine.setValue("[Library]", "MoveFocusBackward", 1);
    }
};

// Library toggle source (note 84)
TakeoutBox.libraryToggleSource = function(_channel, _control, value, _status, _group) {
    if (value === 0x7F) {
        engine.setValue("[Library]", "MoveFocusForward", 1);
    }
};

// Load selected track into the deck matching the MIDI channel (note 96)
TakeoutBox.libraryLoad = function(channel, _control, value, _status, _group) {
    if (value === 0x7F) {
        engine.setValue(TakeoutBox.groupFromChannel(channel),
                        "LoadSelectedTrack", 1);
    }
};

// Toggle library visibility (note 89)
TakeoutBox.libraryToggleVisible = function(_channel, _control, value, _status, _group) {
    if (value === 0x7F) {
        script.toggleControl("[Library]", "show_coverart");
    }
};
