# 800k v2 Quiet-Residual Mini-Match

Clean summary of the completed run. The directory was reused from an earlier
attempt, so `cmd.txt`, `run.log`, and `games.pgn` may contain stale/extra
content. Treat this file as the clean run record.

## Command

```bash
POLICY_FILE=data/policy/v2/run-stockfish-mpv8-d14d15d10-800k-h256/policy_eg10.bin \
MODE=2 \
SCALE=200 \
QUIET_SCALE=200 \
ENDGAME_SCALE=100 \
ROUNDS=50 \
TC='10+0.1' \
HASH=16 \
CONCURRENCY=1 \
TAG=policy_v2_800k_qr_s200_eg100_mini_tc10p0.1 \
./scripts/mini_match_policy_v2.sh
```

## Configuration

- Candidate: `policy_v2_m2_s200`
- Baseline: `policy_off`
- Policy file: `data/policy/v2/run-stockfish-mpv8-d14d15d10-800k-h256/policy_eg10.bin`
- UCI: `UsePolicyV2=true`, `PolicyV2Mode=2`, `PolicyV2Scale=200`, `PolicyV2QuietScale=200`, `PolicyV2EndgameScale=100`
- TC: `10+0.1`
- Threads: `1` per engine
- Hash: `16 MB`
- Concurrency: `1`
- Openings: `tests/openings.epd`
- Mode: fixed gauntlet, no SPRT

## Final Result

```text
Results of policy_v2_m2_s200 vs policy_off (10+0.1, 1t, 16MB, openings.epd):
Elo: 17.39 +/- 43.68, nElo: 27.30 +/- 68.10
LOS: 78.40 %, DrawRatio: 42.00 %, PairsRatio: 1.42
Games: 100, Wins: 22, Losses: 17, Draws: 61, Points: 52.5 (52.50 %)
Ptnml(0-2): [2, 10, 21, 15, 2], WL/DD Ratio: 0.17

Player: policy_off
  Timeouts: 2
  Crashed: 0

Finished match
Total Time: 01:04:09
```

## Conclusion

The candidate is positive but inconclusive. It stays alive, but the mini-match
is not strong enough to skip directly to a long SPRT with high confidence.
Reasonable next step is a normal SPRT only if we accept this as a weak-positive
screen result.
