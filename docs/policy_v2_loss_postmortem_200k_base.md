# Policy v2 — SPRT BASELINE-side post-mortem (where does plain MVV-LVA + history blunder?)

PGN: `data/sprt/policy_v2_mode1_s1000_tc10p0.1/games.pgn` (224 games)  
Candidate: `policy_v2_m1_s1000` (UsePolicyV2=true, Mode=2 quiet_residual, Scale=1000)  
Baseline:  `policy_off` (UsePolicy=false, UsePolicyV2=false)  
Engine for analysis: Stockfish 16 at depth 14 (Hash 256 MB, single-thread, 4 parallel workers)  

## 1. Result breakdown

| candidate color | wins | losses | draws | score |
|---|---:|---:|---:|---:|
| White | 33 | 23 | 56 | 0.545 |
| Black | 23 | 29 | 60 | 0.473 |
| **all** | **56** | **52** | **116** | **0.509** |

(224 games — slight discrepancy from the 224 parsed if any non-decisive results.)

## 2. Phase distribution of deciding blunders (BASELINE's losses = candidate's wins)

| phase | baseline-as-white losses | baseline-as-black losses | total |
|---|---:|---:|---:|
| opening | 0 | 0 | 0 |
| middlegame | 2 | 6 | 8 |
| endgame | 4 | 4 | 8 |
| **classified** | **6** | **10** | **16** |

(40 baseline losses had no single ply with ≥ 200 cp swing — slow positional grinds where the baseline was slightly worse throughout.)

## 3. Top-10 worst BASELINE blunders

| game | baseline color | move | phase | played | swing (cp) | before / after eval (loser POV) | FEN |
|---|---|---|---|---|---:|---|---|
| #62 | Black | 42 | endgame | `Nd8` | 29288 | -705 → -29993 | `1r5k/ppp2p1p/2n5/6Q1/3PpN2/4P1P1/1P3P1P/7K b - - 0 42` |
| #209 | White | 44 | endgame | `Be3` | 29072 | -918 → -29990 | `8/8/p7/2b4p/P3k2P/6P1/4Kp2/8 b - - 0 44` |
| #17 | White | 39 | middlegame | `Bxa7` | 382 | +84 → -298 | `r5k1/pp3p1p/1Bp1nPpq/3rP3/3p4/Q2P4/PPP3PP/3RR2K w - - 6 39` |
| #161 | White | 47 | endgame | `Qh1` | 372 | +192 → -180 | `4rqk1/pp3pb1/7p/4PB2/2nr4/2RN4/PP3P1R/1K2Q3 w - - 2 47` |
| #7 | Black | 33 | middlegame | `Bxf3` | 353 | +343 → -10 | `5r2/p2pk3/1pb1p3/2p5/2P5/3P1P2/P1P3PP/4R2K b - - 6 33` |
| #14 | Black | 16 | middlegame | `h6` | 350 | +321 → -29 | `2k3r1/pp1rbppp/3qpn2/2p1p3/4P2B/2NP2Q1/PPP2PPP/1R2R1K1 b - - 9 16` |
| #60 | White | 81 | endgame | `Rxg5+` | 350 | -1 → -351 | `8/5pkp/7p/6r1/6R1/4PK2/6P1/8 w - - 74 81` |
| #27 | Black | 38 | middlegame | `Qg4` | 319 | +68 → -251 | `2r1r1k1/3p1pp1/7p/1QP1p3/8/pP2PN1q/P4R2/K4R2 b - - 0 38` |
| #167 | Black | 87 | endgame | `Qxc5` | 312 | -212 → -524 | `8/5p1p/4p1p1/2Qq1k2/1P6/K2P2P1/P4P2/8 b - - 24 87` |
| #144 | Black | 22 | middlegame | `Rd7` | 309 | -154 → -463 | `3rr1k1/ppp2ppp/2b1p3/4P1n1/1BB5/4PPK1/PP4PP/2R4R b - - 2 22` |

## 4. Imbalance read (qualitative)

Adjudication breakdown (regex over PGN result comments): 107 resign-style adjudications, 17 draw adjudications.

Average decisive-blunder swing: **5099 cp as White**, **3186 cp as Black** (6 vs 10 blundered games).

Read this against the eval_v2 phase numbers (`docs/policy_v2_eval_100k.md`):
- quiet rank_gain — opening +3.99, middlegame +3.61, **endgame +0.96** (much weaker)
- baseline-rank — `1` and `2` slightly demoted, `6..10` and `11+` strongly promoted

If most of the candidate's losses cluster in **endgame** or **middlegame quiet** decisions, that's consistent with the eval-side weakness. Opening losses would be more surprising and would suggest the policy is corrupting opening tactical choices that baseline gets right with MVV-LVA + history alone.

## 5. One-line recommendation

**Mixed phases.** No single phase dominates — losses look general, not a phase weakness.

