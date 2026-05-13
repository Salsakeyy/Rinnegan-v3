# Policy v2 400k phase tuning

## Scope

This is a measurement-only tuning pass for the 400k v2 policy. No retraining or exporter redesign was done.

Engine-side change:
- Added `PolicyV2EndgameScale`, a v2-only UCI spin option.
- The value is a percent multiplier applied only when the v2 root scorer classifies the root as endgame.
- Default is `100`, so current behavior is preserved.
- The sweep uses `policy_eg10.bin` plus the UCI multiplier instead of exporting additional binaries.

Data and binaries:
- Dataset: `data/policy/v2/labels-stockfish-mpv8-d14d15-400k/dataset.npz`
- 400k policy: `data/policy/v2/run-stockfish-mpv8-d14d15-400k-h256/policy_eg10.bin`
- 200k reference: `data/policy/v2/run-stockfish-mpv8-d14-200k-h256/policy_eg10.bin`
- Raw sweep: `data/policy/v2/run-stockfish-mpv8-d14d15-400k-h256/phase_sweep_400k.csv`

Bench method:
- Engine `bench`, depth 13, 16 fixed positions.
- Threads `1`, Hash `16`.
- `PolicyV2Scale` and `PolicyV2QuietScale` were both set to the listed scale.
- `wall ratio` is derived from engine-reported `nodes / nps`, so use it as a rough timing signal; node counts are the more stable result.

## Baseline and Reference

| config | nodes | node ratio | nps | wall ratio | bestmove delta | feature_ns | forward_ns |
|---|---:|---:|---:|---:|---:|---:|---:|
| off | 3136877 | 1.0000 | 3548503 | 1.0000 | 0 | 0 | 0 |
| 200k_root_bonus_s1000_eg1.00 | 2671655 | 0.8517 | 4029645 | 0.7500 | 2 | 114125 | 2639248 |

## 400k Root Bonus Sweep

| config | nodes | node ratio | nps | wall ratio | bestmove delta | feature_ns | forward_ns |
|---|---:|---:|---:|---:|---:|---:|---:|
| 400k_root_bonus_s400_eg0.00 | 2783589 | 0.8874 | 3619751 | 0.8699 | 3 | 123000 | 2737708 |
| 400k_root_bonus_s400_eg0.25 | 2786598 | 0.8883 | 1976310 | 1.5950 | 3 | 154375 | 9514253 |
| 400k_root_bonus_s400_eg0.50 | 2789728 | 0.8893 | 3957060 | 0.7975 | 3 | 136583 | 2670582 |
| 400k_root_bonus_s400_eg0.75 | 2789728 | 0.8893 | 3685241 | 0.8563 | 3 | 137706 | 2774377 |
| 400k_root_bonus_s400_eg1.00 | 2789728 | 0.8893 | 3729582 | 0.8462 | 3 | 127417 | 2977251 |
| 400k_root_bonus_s600_eg0.00 | 2671655 | 0.8517 | 3465181 | 0.8722 | 2 | 131040 | 2957667 |
| 400k_root_bonus_s600_eg0.25 | 2677792 | 0.8536 | 3937929 | 0.7692 | 2 | 115956 | 2715544 |
| 400k_root_bonus_s600_eg0.50 | 2677794 | 0.8536 | 3514165 | 0.8620 | 2 | 193000 | 2865335 |
| 400k_root_bonus_s600_eg0.75 | 2677794 | 0.8536 | 3222375 | 0.9400 | 2 | 423498 | 3314249 |
| 400k_root_bonus_s600_eg1.00 | 2671655 | 0.8517 | 2273748 | 1.3292 | 2 | 156625 | 3731500 |
| 400k_root_bonus_s800_eg0.00 | 2671655 | 0.8517 | 4047962 | 0.7466 | 2 | 108334 | 2774126 |
| 400k_root_bonus_s800_eg0.25 | 2677794 | 0.8536 | 4164531 | 0.7274 | 2 | 113460 | 2643042 |
| 400k_root_bonus_s800_eg0.50 | 2677794 | 0.8536 | 3787544 | 0.7998 | 2 | 131624 | 2853754 |
| 400k_root_bonus_s800_eg0.75 | 2671655 | 0.8517 | 4274648 | 0.7070 | 2 | 117294 | 2554039 |
| 400k_root_bonus_s800_eg1.00 | 2671655 | 0.8517 | 4351229 | 0.6946 | 2 | 95498 | 2552001 |
| 400k_root_bonus_s1000_eg0.00 | 2671655 | 0.8517 | 4302181 | 0.7025 | 2 | 138795 | 2573166 |
| 400k_root_bonus_s1000_eg0.25 | 2677794 | 0.8536 | 4243730 | 0.7138 | 2 | 106372 | 2591874 |
| 400k_root_bonus_s1000_eg0.50 | 2677794 | 0.8536 | 4291336 | 0.7059 | 2 | 100960 | 2563998 |
| 400k_root_bonus_s1000_eg0.75 | 2671655 | 0.8517 | 4135688 | 0.7308 | 2 | 112503 | 2617166 |
| 400k_root_bonus_s1000_eg1.00 | 2671655 | 0.8517 | 4274648 | 0.7070 | 2 | 106544 | 2609376 |
| 400k_root_bonus_s1200_eg0.00 | 2671655 | 0.8517 | 4330072 | 0.6980 | 2 | 98837 | 2525580 |
| 400k_root_bonus_s1200_eg0.25 | 2677794 | 0.8536 | 4216998 | 0.7183 | 2 | 130003 | 2632413 |
| 400k_root_bonus_s1200_eg0.50 | 2671655 | 0.8517 | 4167948 | 0.7251 | 2 | 113836 | 2651790 |
| 400k_root_bonus_s1200_eg0.75 | 2671655 | 0.8517 | 4267819 | 0.7081 | 2 | 100708 | 2603502 |
| 400k_root_bonus_s1200_eg1.00 | 2671655 | 0.8517 | 4129296 | 0.7319 | 2 | 100709 | 2709999 |
| 400k_root_bonus_s1500_eg0.00 | 2671655 | 0.8517 | 4267819 | 0.7081 | 2 | 103290 | 2601917 |
| 400k_root_bonus_s1500_eg0.25 | 2677794 | 0.8536 | 4126030 | 0.7342 | 2 | 101586 | 2852330 |
| 400k_root_bonus_s1500_eg0.50 | 2671655 | 0.8517 | 4316082 | 0.7002 | 2 | 100208 | 2518375 |
| 400k_root_bonus_s1500_eg0.75 | 2671655 | 0.8517 | 4379762 | 0.6900 | 2 | 96375 | 2522040 |
| 400k_root_bonus_s1500_eg1.00 | 2671655 | 0.8517 | 4274648 | 0.7070 | 2 | 102167 | 2586750 |

## 400k Quiet Residual Sweep

| config | nodes | node ratio | nps | wall ratio | bestmove delta | feature_ns | forward_ns |
|---|---:|---:|---:|---:|---:|---:|---:|
| 400k_quiet_residual_s400_eg0.00 | 2408660 | 0.7679 | 3929298 | 0.6934 | 2 | 120832 | 2731544 |
| 400k_quiet_residual_s400_eg0.25 | 2411669 | 0.7688 | 4087574 | 0.6674 | 2 | 119546 | 2644873 |
| 400k_quiet_residual_s400_eg0.50 | 2414799 | 0.7698 | 4304454 | 0.6346 | 2 | 99957 | 2559249 |
| 400k_quiet_residual_s400_eg0.75 | 2414799 | 0.7698 | 4289163 | 0.6369 | 2 | 122961 | 2537248 |
| 400k_quiet_residual_s400_eg1.00 | 2414799 | 0.7698 | 4243934 | 0.6437 | 2 | 134331 | 2564124 |
| 400k_quiet_residual_s600_eg0.00 | 3136877 | 1.0000 | 4171378 | 0.8507 | 0 | 104750 | 2612418 |
| 400k_quiet_residual_s600_eg0.25 | 3143014 | 1.0020 | 4293734 | 0.8281 | 0 | 99162 | 2564376 |
| 400k_quiet_residual_s600_eg0.50 | 3143016 | 1.0020 | 4317329 | 0.8235 | 0 | 102043 | 2564084 |
| 400k_quiet_residual_s600_eg0.75 | 3143016 | 1.0020 | 4050278 | 0.8778 | 0 | 122252 | 2678041 |
| 400k_quiet_residual_s600_eg1.00 | 3136877 | 1.0000 | 4332703 | 0.8190 | 0 | 105374 | 2666541 |
| 400k_quiet_residual_s800_eg0.00 | 3136877 | 1.0000 | 4011351 | 0.8846 | 0 | 106667 | 2690956 |
| 400k_quiet_residual_s800_eg0.25 | 3143016 | 1.0020 | 3431240 | 1.0362 | 0 | 270586 | 2988290 |
| 400k_quiet_residual_s800_eg0.50 | 3143016 | 1.0020 | 3671747 | 0.9683 | 0 | 142000 | 3192375 |
| 400k_quiet_residual_s800_eg0.75 | 3136877 | 1.0000 | 3980808 | 0.8914 | 0 | 121834 | 2651210 |
| 400k_quiet_residual_s800_eg1.00 | 3136877 | 1.0000 | 3877474 | 0.9152 | 0 | 139333 | 2843084 |
| 400k_quiet_residual_s1000_eg0.00 | 3136877 | 1.0000 | 4026799 | 0.8812 | 0 | 118248 | 2766125 |
| 400k_quiet_residual_s1000_eg0.25 | 3143016 | 1.0020 | 3633544 | 0.9785 | 0 | 114417 | 2901124 |
| 400k_quiet_residual_s1000_eg0.50 | 3143016 | 1.0020 | 3875482 | 0.9174 | 0 | 137334 | 2821041 |
| 400k_quiet_residual_s1000_eg0.75 | 3136877 | 1.0000 | 4042367 | 0.8778 | 0 | 117874 | 2662876 |
| 400k_quiet_residual_s1000_eg1.00 | 3136877 | 1.0000 | 4149308 | 0.8552 | 0 | 106041 | 2652875 |
| 400k_quiet_residual_s1200_eg0.00 | 3136877 | 1.0000 | 4016487 | 0.8835 | 0 | 114709 | 2642290 |
| 400k_quiet_residual_s1200_eg0.25 | 3143016 | 1.0020 | 4087146 | 0.8699 | 0 | 124166 | 2719334 |
| 400k_quiet_residual_s1200_eg0.50 | 3136877 | 1.0000 | 4171378 | 0.8507 | 0 | 110791 | 2609833 |
| 400k_quiet_residual_s1200_eg0.75 | 3136877 | 1.0000 | 3990937 | 0.8891 | 0 | 119498 | 2735916 |
| 400k_quiet_residual_s1200_eg1.00 | 3136877 | 1.0000 | 4011351 | 0.8846 | 0 | 114626 | 2736122 |
| 400k_quiet_residual_s1500_eg0.00 | 3136877 | 1.0000 | 3996021 | 0.8880 | 0 | 140457 | 2750083 |
| 400k_quiet_residual_s1500_eg0.25 | 3143016 | 1.0020 | 3786766 | 0.9389 | 0 | 164919 | 2993040 |
| 400k_quiet_residual_s1500_eg0.50 | 3136877 | 1.0000 | 4105859 | 0.8643 | 0 | 119460 | 2650207 |
| 400k_quiet_residual_s1500_eg0.75 | 3136877 | 1.0000 | 3970730 | 0.8937 | 0 | 115463 | 2726996 |
| 400k_quiet_residual_s1500_eg1.00 | 3136877 | 1.0000 | 3839506 | 0.9242 | 0 | 121248 | 2693168 |

## Best-Move Changes

| config | changed bench positions vs off |
|---|---|
| 400k_root_bonus_s600_eg1.00 | p5: e2d3->e3e4 (640780->257583 nodes); p12: d4d5->f1e1 (309141->324631 nodes) |
| 400k_quiet_residual_s400_eg0.00 | p5: e2d3->e3e4 (640780->208611 nodes); p12: d4d5->c3d5 (309141->238736 nodes) |
| 200k_root_bonus_s1000_eg1.00 | p5: e2d3->e3e4 (640780->257583 nodes); p12: d4d5->f1e1 (309141->324631 nodes) |

## Endgame-Sensitive Positions

Positions 7, 8, 13, and 14 are the sparse/endgame-ish bench positions. Values below are per-position node counts; all listed configs keep the same bestmove as policy-off on these four positions.

| config | p7 nodes | p8 nodes | p13 nodes | p14 nodes | endgame bestmove deltas |
|---|---:|---:|---:|---:|---:|
| 400k_root_bonus_s600_eg1.00 | 23391 | 15756 | 416 | 122718 | 0 |
| 400k_root_bonus_s600_eg0.00 | 23391 | 15756 | 416 | 122718 | 0 |
| 400k_quiet_residual_s400_eg0.00 | 23391 | 15756 | 416 | 137707 | 0 |
| 400k_quiet_residual_s400_eg1.00 | 29315 | 15971 | 416 | 137707 | 0 |

Policy-off reference for those positions:
- p7: 23391 nodes
- p8: 15756 nodes
- p13: 416 nodes
- p14: 122746 nodes

## Read

Root bonus:
- Best node tier is `2,671,655` nodes, ratio `0.8517`, a `14.83%` node reduction vs off.
- Many scale/endgame combinations land on that exact same tree. The lowest main scale that reaches it is `s600`.
- Endgame downscaling does not materially improve root_bonus. At `s600`, `eg0.00` and `eg1.00` both land in the best node tier; intermediate `eg0.25`/`eg0.50`/`eg0.75` are slightly worse.

Quiet residual:
- The useful region is narrow: `s400` only.
- `s400 eg0.00` is the strongest node result: `2,408,660` nodes, ratio `0.7679`, a `23.21%` node reduction vs off.
- `s400 eg1.00` is close at `2,414,799` nodes, ratio `0.7698`.
- Endgame downscaling helps quiet_residual slightly at `s400`, mainly by avoiding the p7/p8 endgame node increase seen at `eg1.00`; it does not change endgame bestmoves on the bench.
- At `s600+`, quiet_residual mostly collapses back to the off tree or slightly worse. It should not be tested at those scales.

Compared with the 200k reference:
- The old 200k reference and the 400k root best tier have the same total node count on this bench, but the 400k sweep shows different behavior at other scales, so this is not a loading issue.
- The 400k quiet_residual `s400` row is a new, materially lower-node candidate than the old 200k reference row.

## Recommendation

Best 400k root_bonus config:
- `PolicyV2Mode=1`, `PolicyV2Scale=600`, `PolicyV2QuietScale=600`, `PolicyV2EndgameScale=100`.
- Reason: lowest scale that reaches the best root_bonus node tier, while preserving default endgame behavior.

Best 400k quiet_residual config:
- `PolicyV2Mode=2`, `PolicyV2Scale=400`, `PolicyV2QuietScale=400`, `PolicyV2EndgameScale=0`.
- Reason: best node count in the sweep and slightly less aggressive on the endgame-ish bench positions than `eg1.00`.

Does endgame downscaling help enough to justify a match test?
- Not for root_bonus. The effect is neutral or slightly negative on this bench.
- For quiet_residual, downscaling from `eg1.00` to `eg0.00` gives a small but consistent node improvement at the only useful scale (`s400`) and reduces some endgame-position node inflation. That is enough to prefer the downscaled quiet_residual candidate over non-downscaled quiet_residual.

Single SPRT candidate:
- Test `policy_eg10.bin` with `PolicyV2Mode=2`, `PolicyV2Scale=400`, `PolicyV2QuietScale=400`, `PolicyV2EndgameScale=0`.
- This is the only 400k config in the sweep that clearly beats the old/root node tier while also limiting endgame policy influence.
