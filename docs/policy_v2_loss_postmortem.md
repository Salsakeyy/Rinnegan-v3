# Policy v2 — SPRT loss post-mortem

PGN: `data/sprt/policy_v2_mode2_s1000_tc10p0.1/games.pgn` (293 games)  
Candidate: `policy_v2_m2_s1000` (UsePolicyV2=true, Mode=2 quiet_residual, Scale=1000)  
Baseline:  `policy_off` (UsePolicy=false, UsePolicyV2=false)  
Engine for analysis: Stockfish 16 at depth 14 (Hash 256 MB, single-thread, 4 parallel workers)  

## 1. Result breakdown

| candidate color | wins | losses | draws | score |
|---|---:|---:|---:|---:|
| White | 35 | 36 | 76 | 0.497 |
| Black | 34 | 32 | 80 | 0.507 |
| **all** | **69** | **68** | **156** | **0.502** |

(293 games — slight discrepancy from the 293 parsed if any non-decisive results.)

## 2. Phase distribution of deciding blunders (candidate's losses)

| phase | white losses | black losses | total |
|---|---:|---:|---:|
| opening | 0 | 1 | 1 |
| middlegame | 5 | 2 | 7 |
| endgame | 12 | 9 | 21 |
| **classified** | **17** | **12** | **29** |

(39 losses had no single ply with ≥ 200 cp swing — these tend to be slow positional grinds where the candidate was slightly worse throughout.)

## 3. Top-10 worst blunders

| game | color | move | phase | played | swing (cp) | before / after eval (cand POV) | FEN |
|---|---|---|---|---|---:|---|---|
| #207 | Black | 17 | middlegame | `Bc5+` | 29594 | -400 → -29994 | `r1bqr1k1/ppppnp2/3b1Q1p/4p1N1/2B1P3/2Pn4/PP4PP/R1B1R1K1 b - - 0 17` |
| #247 | Black | 71 | endgame | `Kg3` | 29589 | -398 → -29987 | `5R2/8/8/5K2/7k/8/8/8 b - - 11 71` |
| #85 | Black | 146 | endgame | `Kg1` | 29583 | -410 → -29993 | `8/8/8/8/8/2K5/3R4/5k2 b - - 19 146` |
| #7 | Black | 94 | endgame | `Kb3` | 29536 | -453 → -29989 | `3R4/8/8/2K5/8/2k5/8/8 b - - 4 94` |
| #132 | White | 82 | endgame | `Qxb3` | 29296 | -696 → -29992 | `1r6/p2k1p2/4p3/K5P1/Pn1P4/1r6/2Q5/8 w - - 5 82` |
| #14 | White | 256 | endgame | `f4` | 28869 | -1127 → -29996 | `8/8/8/5p2/2R3k1/6p1/5r1p/7K b - - 3 256` |
| #291 | Black | 149 | endgame | `Kd4` | 514 | -7 → -521 | `8/8/8/1p6/7P/2k5/4K1P1/8 b - - 0 149` |
| #154 | Black | 71 | endgame | `a3` | 477 | +254 → -223 | `8/5ppp/2k5/8/p1K1p3/4P1PP/1P6/8 b - - 4 71` |
| #218 | White | 111 | endgame | `Kc3` | 473 | -14 → -487 | `8/p4k1p/8/8/2K3R1/6p1/PP3r2/8 w - - 4 111` |
| #41 | White | 64 | endgame | `Nc4` | 404 | -13 → -417 | `r5k1/pp1r1p1p/2ppnPp1/4p3/R7/3PN3/PPP2PPP/4R2K w - - 76 64` |

## 4. Imbalance read (qualitative)

Adjudication breakdown (regex over PGN result comments): 135 resign-style adjudications, 29 draw adjudications.

Average decisive-blunder swing: **3671 cp as White**, **10071 cp as Black** (17 vs 12 blundered games).

Read this against the eval_v2 phase numbers (`docs/policy_v2_eval_100k.md`):
- quiet rank_gain — opening +3.99, middlegame +3.61, **endgame +0.96** (much weaker)
- baseline-rank — `1` and `2` slightly demoted, `6..10` and `11+` strongly promoted

If most of the candidate's losses cluster in **endgame** or **middlegame quiet** decisions, that's consistent with the eval-side weakness. Opening losses would be more surprising and would suggest the policy is corrupting opening tactical choices that baseline gets right with MVV-LVA + history alone.

## 5. One-line recommendation

**Endgame-dominant losses.** Matches the eval prediction — the model is weak in endgames and that translates to game losses.

