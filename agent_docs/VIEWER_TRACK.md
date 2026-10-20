# Viewer track

Visual-only state a replay viewer draws and a recording does not carry: what
the game's effects, colors, stage parts and camera were doing. Gameplay never
reads it. A viewer outside this repository wraps the core, turns the track on,
and draws from it; the core only reports.

## The switch

`MslCoreMatchConfig.visual_only` (runtime/wire.h) holds `MSL_VISUAL_*` bits.
`MSL_VISUAL_TRACK` (bit 0) is the only one. Off by default: every public path
(the batch API, validation, benchmarks) leaves it clear.

With the bit clear the core is upstream main's: no allocation, no extra load
work, nothing written. Each hook in game code is one untaken branch on a
thread-local pointer the core already publishes at match bind, beside the
others there.

With the bit set, `msl_core_match_init` gives the match a track (heap memory
outside the match arena, freed with the match) and every step ends by
appending the frame's lines to it. `msl_core_match_viewer_track(match, &size)`
returns the lines written since the last call. The track reads simulation
state; it does not change gameplay (the locks below check the strict gameplay
pass and the same output fingerprint with the track off).

The random state lines and seeds need effect data gameplay does not load
(runtime/effects.c): load the game data with `msl_core_game_data_init_visual`
for a match with the bit set; `msl_core_match_init` refuses the bit without
it. That load adds every effect file of the game's list that gameplay leaves
out, every file's model table, and one counted animation step per effect
model. `msl_core_game_data_init` does none of it. `msl_viewer_create`
(below) loads it.

Limits: one track per match. A savestate restore or match copy keeps the
destination's own track (or none); a track is not part of a savestate. The
batch API has no way to turn it on. The validation runner writes it only in
its one-shot stream mode.

## The viewer API

A standalone viewer wraps the shared library through `api.h`'s
`msl_viewer_*` functions, which replay one recording the way the validation
runner does (`runtime/main.c --stream`) and keep the track on:

- `msl_viewer_create(data_root, config, config_size, previous_input,
  input_size, &viewer)` takes the runner's header rows (the
  `MslCoreMatchConfig` wire row and the `MslCoreInput` before the first
  frame), sets `MSL_VISUAL_TRACK`, loads the viewer's own game data and
  initializes the match;
- `msl_viewer_step(viewer, frame, frame_size)` steps one `MslCoreStreamFrame`
  wire row (the frame's seed, inputs and stage events);
- `msl_viewer_track(viewer, &size)` returns the lines written since the last
  step (after create: the match's setup);
- `msl_viewer_output(viewer, &size)` returns the step's `MslCoreCompare`, the
  row the runner writes;
- `msl_viewer_destroy(viewer)` frees the match and its game data.

## Lines

One JSON array per line: `[frame, kind, values...]`. `frame` is the game's
frame number (Slippi's: -123 is the first). A line written during a step
carries the frame that step produces; one written while the match is built
carries the first frame.

Two sorts of kind:

- **States** are written on the first frame they have a value and whenever it
  changes. The identity fields after the kind (a port, a slot) and the kind
  make the key; a viewer keeps the last value per key.
- **Events** are written when they happen, in the order the game did them.

Within a frame the step's events come first, in the game's order, then what
is read at the frame's end: each fighter's states, then the stage's, the
items', and the last rows of the table. Numbers are C `%.6g` unless the table
says otherwise. `port` is the fighter's port (0-3) and `sub` 0 for the leader, 1
for Nana.

| Kind | Sort | Values | Source |
|---|---|---|---|
| `tint` | state | port, sub, slot (0-2), index, rgba, flags, light rgba, light x, light yz; or index 0 when the overlay stops | `Fighter.x408/x488/x508` color overlays |
| `charge` | state | port, sub, field, value | stored charges: `dk`, `ss`, `mt`, `sk`, `gw`, Kirby's `kb.kind`, `kb.dk`, `kb.ss` |
| `part` | state | port, sub, model part, variant | `Fighter.x5F4_arr[i].idx` |
| `anim` | state | port, sub, animation index, source kind, frame, rate, blend frames, blend frame | the animation the game plays, and whose table it is |
| `pos` | state | port, sub, x, y, z, rot x, y, z, scale | the top joint, while in DeadUp states (4-10) or off the plane |
| `shake` | state | port, sub, x, y, z (0 when it stops) | the draw offset `ftCommon_8008021C` sums: hit shake, the held-mash wobble, the smash-charge shake |
| `hide` | state | port, sub, bits (1 invisible, 2 `x221E_b5`, 4 `x2226_b5`) | written once something is hidden |
| `bg` | state | state, current, previous | Battlefield's background (map part 3) |
| `fd` | state | phase, x, y, z, frames | Final Destination's background (map part 3) |
| `fdrot` | state | rot x, y, z | Final Destination part 7's turn in phases 9 and 10 |
| `fod` | state | left height, right height | Fountain of Dreams' water columns (`grIzumi_801CC358`) |
| `gcolor` | state | stage object, rgba (0 when it stops) | stage material color scripts |
| `bgcolor` | state | rgba | the camera's background color |
| `cam` | state | eye x, y, z, interest x, y, z, fov, quake x, y (`%.2f`) | the game camera without its quake, and the quake |
| `hook` | event | port, sub, x, y pairs (`%.2f`) | each frame a hookshot, grapple or yo-yo chain is out: tip first, links not yet out left out |
| `art` | state | item kind, owner's character, article index (-1 none) | which of the owner's articles an item kind draws, once per kind |
| `irot` | state | item id, rot x, y, z (`%.4f`) | an item's root joint turn, once it turns |
| `ij` | state | item id, joint, offset x, y, z (`%.4f`), 3x3 matrix | an item joint a generator was made on: place from the item's and its turn, at the frame's end |
| `rng` | state-like, every frame | random state | the state after the frame's gameplay, where the game's particle update starts |
| `tex` | event | port, sub, slot, index (slot -1: every slot back) | switchable textures, eyes and mouth (`ftAnim_800704F0`, `ftAnim_80070654`) |
| `fx` | event | effect id, owner kind (`f` fighter, `i` item, `s` stage, `-`), owner id, joint, x, y, z, a0, a1, seed | each effect spawn (`efSync_Spawn`, `efAsync_Spawn`) with its arguments |
| `fxend` | event | owner kind, owner id | `efLib_DestroyAll` |
| `fxp` | event | effect id, owner kind, owner id, index, `m` or `g`, id, how, joint, t xyz, r xyz, s xyz, callback, lifetime, params xyz, mirrored, at xyz, seed[, item id, item joint] | one per model or generator a spawn's dispatch makes, after its `fx` |
| `gen` | event | bank, generator, x, y, z, seed, stage object, joint[, owner kind, owner id[, item joint]] | a joint animation's particle key (`efLib_Cb_DPtcl`) |
| `sgen` | event | running stage object, bank, generator, x, y, z, seed, stage object, joint[, mark] | a generator the stage's code makes (`grLib_801C96F8`, `801C97DC`, `801C9808`); mark `on` (made on the joint) or `kept` (kept to end later) |
| `send` | event | how, stage object, joint, joints | the stage's code ending generators (`grLib_801C9834` .. `801C9908`) |
| `ganim` | event | stage object, animation set, animation, start frame, rate | a stage animation's start (`grAnime_801C8138`) |
| `stadscreen` | event | screen, followed player (-1 but for a close-up, 8) | Pokemon Stadium's screen changes |
| `bird` | event | row, count, x, y, z of the first | Dream Land's background birds |

A stage joint is named by its stage object's number (`Ground_801C2BA4`) and
its preorder index in that object's tree (root 0, an instance's children left
out). An object the stage's table does not hold is its number plus 1000 for
each object of that number made before it: Fountain of Dreams' left column is
1004.

With the track on, Dream Land's background runs the game's whole animation
(gameplay runs only its particle tracks, which draw the same), and Whispy's
blow effect plays the animation of GALE01's table (`0x804D48E0`, `{ 1, 0 }`).

## Seeds

A seed is the game's random state where the line's thing was made.
A spawn's pieces are seeded in the game's order: each generator as the
dispatch makes it, then the models' first animation steps, the last made
first (`efLib_AnimQueue`).
The simulator leaves out some draws the game makes (a generator of a file
gameplay does not load, a fighter file model's first animation step, the
stage's own generators): the track counts them, so a seed and `rng` are the
game's state, not only the simulator's.

## Validation

`replays/suites/visual_state.json` lists recordings whose viewer track is
known to be the game's: their effects matched the game's own particle code
frame for frame and their poses were checked against the game. For each,
`replays/suites/visual_state_locks.json` stores digests of its track
(blake2b-64): all lines, each kind's lines, and each minute's lines.
`tests/test_visual_state_locks.py` replays each with the track on through the
validation runner (`validate_replay(..., visual_only=MSL_VISUAL_TRACK)`),
requires the strict gameplay pass and the same output fingerprint as with the
track off, and compares the digests. `python -m tools.validation.visual_state`
checks them; `--write` records them.

The locks are recorded on the Linux reference build, like the gameplay
output locks: `python -m tools.validation.visual_state --write`.

### Unlocked kinds

A kind is locked only once a recording proves it: its lines matched the
game's frame for frame. A kind the track writes that no recording of the suite
proves yet is unlocked: `visual_state.UNLOCKED_KINDS` leaves its lines out of
every digest (the whole track's, each kind's, each minute's), so a change to
it fails nothing.

Unlocked:

- `fdrot` (Final Destination's background turn): its value is the game's, but
  no recording was compared against the game's own background;
- `hook` (the grapple and hookshot chain links): no recording of the suite
  checks the links against the game's;
- `send` (the stage's own code ending generators) and `stadscreen` (Pokemon
  Stadium's screen): no recording of the suite checks them.
