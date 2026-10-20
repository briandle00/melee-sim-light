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

- PR #60 review: reuse the source Stadium display scheduler without rendering,
  deleting its duplicated implementation. Final mixed timing is +1.40%,
  non-Stadium −0.92%; earlier Stadium-only timing is −0.43%. Measurements are
  variable and do not establish zero cost. All 520 replay locks pass.
  [Measurement details](BASELINE.md#stadium-display-schedule-review).

- PR #60/#45 cost screening (2026-10-19): against main `b2eb33d4`, submitted
  #60 measures +0.18% mixed and +0.19% Stadium throughput; #45 with arithmetic
  compatibility disabled measures +0.44% in the repeat comparison. No meaningful
  slowdown observed; these small differences are within run variation, not
  speedup claims. Default #45 outputs match main. #60 changes RNG/gameplay.
  [Measurement details](BASELINE.md#replay-only-build-option-cost-screening-2026-10-19).

- PR #60 integration: the initial source reuse cleanup regressed mixed FPS by
  0.76%, then 1.03% on repeat, and was held. Retain a narrower refinement that
  combines identical timer cases and inlines the two display helpers under the
  existing compiler profile. Final original-workload eight-pair comparison:
  123,417 versus main 123,026 FPS (+0.318%), paired 95% interval
  [-0.163%, +0.802%]. Extended mixed and Stadium checks show no slowdown;
  523 replay locks / 4,830,487 frames, native/Wasm and 151 tests pass.
  [Evidence and limits](BASELINE.md#stadium-scheduler-refinement-2026-10-20).
