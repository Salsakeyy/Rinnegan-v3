# Policy v1 — SPRT validation @ PolicyScale 160

> Real-game validation of the AVX2/NEON-optimized v1 policy against
> `UsePolicy=false`. No model change, no retrain. Companion to
> `docs/policy_v2_quiet_residual_test.md`.

## Setup

| Field             | Value                                                             |
|-------------------|-------------------------------------------------------------------|
| Engine binary     | `chess-engine/build/engine` (post-AVX2/NEON optimization)         |
| Policy binary     | `data/policy/new32-10m-f32-h512-runpod/h512/train/smoke-policy.bin` |
| Candidate options | `UsePolicy=true PolicyMode=1 PolicyScale=160`                     |
| Baseline options  | `UsePolicy=false`                                                 |
| Time control      | 10 + 0.1 (10 s + 100 ms increment)                                |
| Hash              | 16 MB                                                              |
| Threads           | 1 per side                                                         |
| Concurrency       | 4                                                                  |
| Openings          | `tests/openings.epd` (40 EPD lines, random order)                 |
| Adjudication      | draw movecount=8 score=8 from move 40; resign movecount=4 score=800 |
| SPRT bounds       | elo0=0  elo1=5  α=β=0.05                                          |
| Round cap         | 500 rounds (= 1000 games)                                         |

Exact command (also saved at `data/sprt/policy_mode1_s160_tc10p0.1/cmd.txt`):

```bash
ROUNDS=500 TC=10+0.1 ELO0=0 ELO1=5 \
  bash chess-engine/tests/sprt_policy_vs_off.sh
```

## Result

<!-- Filled in after the run completes. -->

| Field            | Value          |
|------------------|----------------|
| Total games      | _TBD_          |
| Wins (candidate) | _TBD_          |
| Losses           | _TBD_          |
| Draws            | _TBD_          |
| Score            | _TBD_          |
| Elo (candidate − baseline) | _TBD_ ± _TBD_ |
| LOS              | _TBD_          |
| nElo             | _TBD_          |
| LLR              | _TBD_          |
| SPRT decision    | _TBD_          |

Logs:

- `data/sprt/policy_mode1_s160_tc10p0.1/run.log`
- `data/sprt/policy_mode1_s160_tc10p0.1/games.pgn`

## Conclusion

<!-- Filled in after the run completes. -->

## Follow-up scale check

<!-- Filled in if 120 / 200 sweep is run. -->
