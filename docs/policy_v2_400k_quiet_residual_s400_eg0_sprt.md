# Policy v2 400k quiet-residual s400 eg0 SPRT

## Setup

Candidate:
- Policy file: `data/policy/v2/run-stockfish-mpv8-d14d15-400k-h256/policy_eg10.bin`
- `UsePolicyV2=true`
- `PolicyV2Mode=2`
- `PolicyV2Scale=400`
- `PolicyV2QuietScale=400`
- `PolicyV2EndgameScale=0`

Baseline:
- Same engine binary
- `UsePolicyV2=false`
- `UsePolicy=false`

Harness:
- `tests/sprt_policy_vs_off.sh`
- Minimal harness change: added `ENDGAME_SCALE` env support for v2, defaulting to `100`.

## Command

```bash
USE_V2=1 \
POLICY_FILE="$PWD/data/policy/v2/run-stockfish-mpv8-d14d15-400k-h256/policy_eg10.bin" \
MODE=2 \
SCALE=400 \
ENDGAME_SCALE=0 \
TC='10+0.1' \
HASH=16 \
ROUNDS=1000 \
OUT_DIR="$PWD/data/sprt/policy_v2_400k_qr_s400_eg0_tc10p0.1" \
bash tests/sprt_policy_vs_off.sh
```

The exact `fastchess` command is saved at:
- `data/sprt/policy_v2_400k_qr_s400_eg0_tc10p0.1/cmd.txt`

## Match Conditions

- Runner: `fastchess`
- Time control: `10+0.1`
- Hash: `16 MB`
- Threads: `1`
- Concurrency: `4`
- Openings: `tests/openings.epd`, random order
- Pairing: `-rounds 1000 -games 2 -repeat`
- Draw adjudication: `movenumber=40 movecount=8 score=8`
- Resign adjudication: `movecount=4 score=800`
- SPRT: `elo0=0 elo1=5 alpha=0.05 beta=0.05`

Outputs:
- Log: `data/sprt/policy_v2_400k_qr_s400_eg0_tc10p0.1/run.log`
- PGN: `data/sprt/policy_v2_400k_qr_s400_eg0_tc10p0.1/games.pgn`

## Stop Status

The run was manually stopped at user request before a formal SPRT accept/reject boundary.

Completed PGN games at stop:

| games | wins | losses | draws | points | score | simple Elo |
|---:|---:|---:|---:|---:|---:|---:|
| 154 | 39 | 45 | 70 | 74.0 / 154 | 48.05% | -13.54 |

Candidate by color:

| candidate color | wins | losses | draws |
|---|---:|---:|---:|
| White | 22 | 24 | 31 |
| Black | 17 | 21 | 39 |

Termination tags in the PGN:

| termination | games |
|---|---:|
| adjudication | 77 |
| normal | 65 |
| time forfeit | 12 |

## Last Fastchess Checkpoint

The last full fastchess SPRT summary before interruption was at 140 games:

```text
Results of policy_v2_m2_s400 vs policy_off (10+0.1, 1t, 16MB, openings.epd):
Elo: -22.37 +/- 39.19, nElo: -33.07 +/- 57.55
LOS: 13.00 %, DrawRatio: 41.43 %, PairsRatio: 0.71
Games: 140, Wins: 33, Losses: 42, Draws: 65, Points: 65.5 (46.79 %)
Ptnml(0-2): [5, 19, 29, 14, 3], WL/DD Ratio: 0.81
LLR: -0.20 (-6.9%) (-2.94, 2.94) [0.00, 5.00]
```

SPRT status at that checkpoint:
- Not accepted.
- Not rejected.
- LLR was negative and moving against the candidate.

## Read

The fixed-depth bench result did not survive cleanly into games. The candidate was negative at every meaningful checkpoint after the opening sample:

| checkpoint | W-L-D | score | Elo / CI | LOS | LLR |
|---:|---|---:|---|---:|---:|
| 60 games | 15-18-27 | 47.50% | -17.39 +/- 65.85 | 29.99% | -0.06 |
| 80 games | 18-22-40 | 47.50% | -17.39 +/- 52.73 | 25.71% | -0.09 |
| 100 games | 21-29-50 | 46.00% | -27.85 +/- 48.15 | 12.59% | -0.17 |
| 120 games | 25-36-59 | 45.42% | -31.94 +/- 41.06 | 6.17% | -0.25 |
| 140 games | 33-42-65 | 46.79% | -22.37 +/- 39.19 | 13.00% | -0.20 |

The PGN at manual stop was less bad than the 120-game checkpoint but still negative:
- 39 wins, 45 losses, 70 draws
- 48.05% score
- about -13.5 simple Elo

## Conclusion

This config is not formally dead by SPRT because the run was stopped long before a boundary. Practically, it should not stay alive as the lead candidate:

- It started negative and stayed negative through the latest fastchess checkpoint.
- LOS was only 13% at 140 games.
- The LLR was still close to zero in absolute terms, but on the wrong side.
- The bench node win appears not to translate into real-game strength at this time control/opening set.

Recommendation:
- Do not continue this exact `Mode=2 Scale=400 EndgameScale=0` SPRT as the main candidate.
- Treat it as dead for match-testing priority, unless there is a separate reason to investigate the time-forfeit/no-output behavior.
