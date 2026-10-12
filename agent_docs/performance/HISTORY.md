# Performance changes

- PR #67: load only Dream Land particle event tracks, preserving their source
  clocks and RNG order. The final paired comparison measured 121,624 FPS versus
  main's 121,664 FPS; the earlier candidate measured 119,361 FPS. All 517 replays
  (4,768,869 frames), output locks, native checks and Wasm parity pass.
  [Measurement details](BASELINE.md#particle-animation-repair).
