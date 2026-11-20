# Playback priority: game vs music

## Context

RamkolFX has one EQ curve for the whole mix: everything plays through
one virtual sink. When a game and a music player play at the same time,
one binding has to win, and the game and the music get the same curve.

What happens today (phase 6 of `docs/content-detect-plan.md`, not yet
released) depends on the kind of binding, and the two kinds disagree:

- **App bindings:** the bound app that started playing most recently
  wins. A game counts as playing for as long as its stream is open, so
  its "start" is when it launched. Music started after the game wins.
  But if the game reopens its stream (a level load longer than the 2 s
  grace period), the game becomes the newest app and takes over, even
  though the music was there first. In practice, which one wins looks
  random.
- **Device Auto binding:** "if you're gaming, the game wins". But a
  bound music app still wins, because app bindings are checked before
  the device binding. So the game only wins when nothing else is bound.

Goal: one rule that the user can predict, with the choice of who wins
left to the user.

## Decision

A single global setting, **Priority**, with three choices:

| Choice | What it means | Who picks it |
|--------|---------------|--------------|
| **Game first** (default) | While a game is open, its binding decides. Bound music and video apps wait. | Most gamers: footsteps and positional cues need the game's curve, and music played alongside a game is background. |
| **Music first** | A bound app that is playing takes over from the game. The game's curve comes back 2 s after that app stops. | Gaming with a playlist that should sound like music. |
| **Latest first** | Whichever started most recently wins. This is today's rule for app bindings. | People who change things by hand and want the newest app to win. |

The default is Game first: a game's preset is built for the game, and
when a game is open the user is almost always playing it.

Priority only affects conflicts between a game and something else.
Two non-game apps always use Latest first, as they do today. Two games
also use Latest first.

### Rejected

- **A ranked list of kinds** (Game > Music > Video > Voice). RamkolFX
  can't reliably tell what kind of app something is. User presets have
  no category, so a binding to one doesn't say what the app plays. The
  classifier hears the whole mix, so while two apps play it can't tell
  which one is the music.
- **Blending the two curves** (averaging the gains). The result fits
  neither the game nor the music, and bass-enhancer settings can't be
  averaged in a way that means anything.
- **Following the focused window.** On Wayland, apps can't see which
  window has focus, and the game is usually the focused window anyway.
- **A priority setting on every app.** This is more than most people
  need. If Music first turns out to be too coarse (Spotify should beat
  the game but a YouTube tab shouldn't), add it later as a "Wins over
  games" flag on each app (see Later).

## Behavior

**Contenders.** These are the apps that take part in the choice:

- Every playing app that has a binding (a preset or Auto-detect). A
  game counts as playing while its stream is open; any other app counts
  as playing for 2 s after it goes quiet, as today.
- When the output device is bound to Auto-detect, every playing game,
  even an unbound one. It stands in for the device's Auto binding with
  `bGame` set: `{Device, <output device>, "", bAuto, bGame}`.

Unbound non-game apps never take part, as today. Binding an app is how
it enters the contest.

**Choosing a winner:**

1. Game first: games come before non-game apps.
2. Music first: non-game apps come before games.
3. Latest first: no ordering by kind.
4. Between apps of the same kind, the most recent playing spell wins
   (`StartedAtMs`). On equal start times, the app first in alphabetical
   order wins (unchanged).

If there are no contenders, the device binding applies (a preset, or
Auto-detect without a game), then the manual curve. This is unchanged.

**What stays the same.** A change of winner is a normal binding change,
so everything that already handles one keeps working:

- The pre-binding snapshot and restoring it.
- A new binding ends a hold. Under Music first, a tweak made while the
  music won is dropped when the game takes back over.
- A fresh detection session starts for the new winner.

The edge-triggered switcher applies the new preset once.

**Music first and the device Auto binding.** Only *bound* apps take
over from a game. An unbound Spotify playing next to a game under a
device Auto binding doesn't change anything. RamkolFX doesn't run the
classifier on the game-plus-music mix to look for music: game
soundtracks and voice chat would make that guess wrong. The GUI says so
(see GUI).

**Unbound games.** Under a device preset binding (not Auto), an unbound
game isn't a contender. It doesn't need to be: there is no game-specific
curve to apply.

### Scenarios (unit-test table)

The steps in each row happen in order. `G` = game, `S` = Spotify (not a
game), `D` = Discord (unbound). All apps are playing unless the row says
otherwise.

| # | Bindings | Steps | Game first | Music first | Latest first |
|---|----------|--------|-----------|-------------|--------------|
| 1 | G: FPS, S: Rock | G starts, then S | G (FPS) | S (Rock) | S (Rock) |
| 2 | G: FPS, S: Rock | S starts, then G | G | S | G |
| 3 | G: FPS, S: Rock | 1, then G reopens its stream after 3 s | G | S | G |
| 4 | G: FPS, S: Rock | 1, then S pauses for longer than 2 s | G | G | G |
| 5 | device Auto, G unbound, S: Rock | G starts, then S | device/Game (Action) | S (Rock) | S (Rock) |
| 6 | device Auto, G unbound, S unbound | G and S play | device/Game | device/Game | device/Game |
| 7 | device Auto, G unbound, D unbound | G and D play | device/Game | device/Game | device/Game |
| 8 | device Rock, G unbound, S unbound | G and S play | device (Rock) | device (Rock) | device (Rock) |
| 9 | G: Auto, S: Auto | G starts, then S | G (Game class) | S (classifier) | S (classifier) |
| 10 | S: Rock, Firefox: Movie | S, then Firefox | Firefox | Firefox | Firefox |

Row 5 changes the current behavior: under Game first, a game now beats
a bound music app under a device Auto binding. That is what the
comment in `ResolveBinding` already intends. Game detection hasn't been
released yet, so no user will notice the change.

Row 9 under Music first: the classifier hears the game as well as the
music. That is the known whole-mix limit, and it's no worse than today.

## GUI

The setting goes in the **App preset** card, next to who is currently
winning: that's where the user looks when the wrong curve plays. The card
has a fixed height and sits beside the device card, so it keeps five
rows. The combo replaces the "N playing" count on the header row (which
stays for a daemon too old to have the setting), and has no vertical
frame padding so the row keeps its height.

```
┌ App preset ──────────────── Priority [Game first ▾] ┐
│ [ Spotify                                       ▾ ] │
│ [ Rock                                          ▾ ] │
│ Waiting: ArmaReforgerSteam has priority             │
│ Waiting: Spotify                                    │
└─────────────────────────────────────────────────────┘
```

- **Status line** (fourth row). For the selected app, when it is a
  contender that lost: "Waiting: <winner> has priority" ("the game"
  when an unbound game wins through the device binding).
- **"Waiting:" line** (fifth row). It lists the contenders that lost:
  playing apps with a binding (and games, under a device Auto binding),
  other than the winner. It is only shown when something is waiting.
  Each name is a link that selects that app in the combo above.
- **Unbound-music hint.** Under Music first with a game in effect, if
  an unbound non-game app is playing and nothing is waiting, the fifth
  row shows "Spotify isn't bound: bind it to beat the game". The name
  links to selecting that app. This covers row 6, the one case where
  the setting seems to do nothing.
- **Tooltips.** On the combo: "Only one EQ curve plays at a time. This
  decides whose it is when a game and another bound app play together."
  Each choice in the list explains itself.
- **Tray menu** (phase 4): a "Priority" submenu with the three choices
  as radio items, so the setting can be changed in the middle of a game
  without opening the window.
- **Settings popup:** an "EQ auto-switch" section with the same combo
  ("When a game and music play together") and a line on what it does.
- **One value, three views.** The card's combo, the Settings combo and
  the tray submenu all show the daemon's status (`LastStatus.Priority`)
  and change it only through `App::SetPlaybackPriority`. The tray is fed
  from it on every pass of the main loop, so a change made in any of
  them shows in the others straight away. Without a daemon that has the
  setting, the combos are disabled and say why.
- The EQ preset status line (`- auto (Firefox)`) already names the
  winning app and needs no change.

## Protocol and storage

- **Enum.** Add `enum class PlaybackPriority : uint8_t { GameFirst = 0,
  MusicFirst = 1, Latest = 2 }` to `protocol.hpp`. A value from a newer
  daemon is kept as sent: the GUI shows it as "Unknown", checks no
  choice, and lets the user pick any of them.
- **Storage.** `settings.conf` gets `auto_switch.priority=game|music|latest`,
  next to the `auto_detect.*` timing values. If the key is missing or
  unknown, the value is `game`.
- **Wire.**
  - A new opcode, `SetPlaybackPriority [u8]`.
  - The state reply gets two more trailing blocks, each with the usual
    `bHas...` flag so a GUI can tell when a daemon is too old to send
    it:
    - the priority byte (`bHasPlaybackPriority`); without it the GUI
      hides the card's combo and disables the Settings one;
    - the contest (`bHasContest`): the winning contender's app name, whether
      it's a game, and the waiting contenders' names.
  - An older GUI ignores the trailing blocks. An older daemon doesn't
    send them.
- **GUI data.** The "Waiting:" line and the Music-first hint come from
  the contest block: the daemon reports who won and who waits, so the
  GUI never repeats the rule (the grace period, which apps are bound,
  Auto-detect being available). Only the hint's "is this playing app
  unbound?" is read from the stream list.

## Code

- **Resolver.** `ResolveBinding(...)` in `preset_auto_switch.*` takes a
  `PlaybackPriority`.
  - It builds the contender list, then picks one with a single
    comparator: (kind rank by priority, `StartedAtMs`, name).
  - The device-Auto game contender uses the game's `StartedAtMs`.
  - Whether an app is a game is kept on its `AppActivityTracker` entry
    and lasts through the grace period, like its playing state. A game
    whose stream closes for a moment still ranks as a game, rather than
    losing to music for the length of the gap.
  - It also reports the contest (`BindingContest`: winner, whether it's
    a game, the losers). This is kept apart from `ResolvedBinding`, whose
    equality is the binding's identity.
  - Bound winners resolve exactly as today. The device-Auto game
    contender resolves to the device binding with `bGame`.
- **Session and switcher.** `AutoDetectSession` and the switcher don't
  change.
- **Engine.** `AudioEngine` passes the stored priority on each 250 ms
  tick. Changing the setting is applied on the next tick and acts like
  any other binding change.

## Phases

1. **Resolver logic and tests.** *(Done.)*
   - Add `PlaybackPriority` and the contender rule to `ResolveBinding`.
   - Cover the scenario table above in a unit test, including the
     device-Auto game contender and equal start times.
2. **Storage, protocol and engine.** *(Done.)*
   - Add the `settings.conf` key, the opcode and the trailing state byte,
     with encode and decode tests in both directions, old and new.
   - Wire it into `AudioEngine`.
3. **GUI.** *(Done, not yet looked at on screen with a real game.)*
   - Add the Priority combo, the "Waiting:" line, the unbound-music hint
     and the tooltip in the App preset card.
   - Look at it on screen with a game and Spotify running.
   - Add a CHANGELOG entry under Unreleased, together with game
     detection.
4. **Tray submenu.** *(Done; checked over D-Bus, not yet in a real
   panel.)* "Priority" between Show/Hide and Quit, with the three
   choices as dbusmenu radio items.
   - The GUI doesn't talk to the daemon while hidden, and one started
     hidden has never connected. So opening the menu (`AboutToShow`)
     requests the status alone, at most once a second (hosts send
     `AboutToShow` per submenu too), connecting first if needed; the
     rest of the UI's state is fetched once the window draws again.
     Changes go out as `LayoutUpdated`. Until the daemon has been heard
     from, the submenu shows with nothing checked, and a click connects
     first.
   - Nothing polls while hidden, so the connection may be dead (the
     daemon restarted): a failed request reconnects once and retries.
   - Against a daemon too old for the setting, a click sends nothing.
   - Hidden only for a daemon known to predate the setting.

**Later, if asked for:**

- A per-app "Wins over games" flag. It would take over the job of Music
  first, for setups where only some apps should beat the game.
- A longer delay before the game takes back over under Music first. Use
  it if pausing and resuming music between tracks flips the EQ: some
  players close their stream between tracks.
