# Performance changes

- PR #67: load only Dream Land particle event tracks, preserving their source
  clocks and RNG order. The final paired comparison measured 121,624 FPS versus
  main's 121,664 FPS; the earlier candidate measured 119,361 FPS. All 517 replays
  (4,768,869 frames), output locks, native checks and Wasm parity pass.
  [Measurement details](BASELINE.md#particle-animation-repair).

- PR #58: retain static blending for unanimated gameplay bones and exclude the
  existing presentation-only placeholder. Shield-heavy cost falls from 2.9%
  to 0.68% relative to main; the final mixed check is −0.73%. All 518 replay
  locks, native checks and Wasm parity pass. General math experiments reverted.
  [Measurement details](BASELINE.md#presentation-only-joint-exclusion).

- PR #43: select FD's frozen-background call suppression from recorded Slippi
  instructions, including benchmark exports. Mixed/default timing is −0.47%
  and FD/default −0.40%, within the short-run spread; all 519 replay locks,
  native checks and Wasm parity pass.
  [Measurement details](BASELINE.md#frozen-fd-replay-capability).
