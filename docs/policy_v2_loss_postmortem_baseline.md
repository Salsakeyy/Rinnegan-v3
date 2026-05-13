# Policy v2 — SPRT BASELINE-side post-mortem (where does plain MVV-LVA + history blunder?)

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

## 2. Phase distribution of deciding blunders (BASELINE's losses = candidate's wins)

| phase | baseline-as-white losses | baseline-as-black losses | total |
|---|---:|---:|---:|
| opening | 0 | 2 | 2 |
| middlegame | 4 | 6 | 10 |
| endgame | 9 | 6 | 15 |
| **classified** | **13** | **14** | **27** |

(42 baseline losses had no single ply with ≥ 200 cp swing — slow positional grinds where the baseline was slightly worse throughout.)

## 3. Top-10 worst BASELINE blunders

| game | baseline color | move | phase | played | swing (cp) | before / after eval (loser POV) | FEN |
|---|---|---|---|---|---:|---|---|
| #9 | White | 32 | middlegame | `g5+` | 29623 | -370 → -29993 | `6k1/pp2rp1p/2p3p1/2P5/4PK2/Q3RP2/PP3P1P/5q2 b - - 0 32` |
| #104 | Black | 73 | endgame | `a6` | 29099 | -887 → -29986 | `3k4/p4p2/2BP1K2/8/5P2/4P3/8/8 b - - 2 73` |
| #225 | White | 128 | endgame | `c3` | 892 | +483 → -409 | `8/1p2k3/8/4KPP1/p7/8/2P5/8 w - - 0 128` |
| #292 | White | 37 | middlegame | `Nf4` | 420 | +217 → -203 | `8/1p3ppp/p3pk2/2n1r3/2R5/8/PP2NPPP/5K2 w - - 6 37` |
| #70 | Black | 53 | endgame | `Qd5` | 419 | -0 → -419 | `K7/1P3ppp/R3pk2/2q5/8/4P3/PP3PPP/8 b - - 14 53` |
| #99 | Black | 67 | endgame | `f4` | 400 | +337 → -63 | `8/1p6/p1p5/5pk1/8/5K2/PP5P/8 b - - 3 67` |
| #164 | White | 93 | endgame | `f4` | 366 | +0 → -366 | `8/7p/8/8/7P/3n2P1/4kPK1/8 w - - 3 93` |
| #114 | Black | 119 | endgame | `Qe7` | 360 | -0 → -360 | `1Q6/pp5p/4p1k1/3b2qp/3P3R/6P1/5P1K/8 b - - 43 119` |
| #73 | White | 120 | endgame | `Kh2` | 338 | -53 → -391 | `1Q6/p4p2/4p2p/4Pkp1/8/P3q2P/1P4P1/7K w - - 6 120` |
| #253 | Black | 83 | endgame | `Kb6` | 323 | -0 → -323 | `3r4/ppk5/8/8/8/5KP1/5P1P/4R3 b - - 63 83` |

## 4. Imbalance read (qualitative)

Adjudication breakdown (regex over PGN result comments): 135 resign-style adjudications, 29 draw adjudications.

Average decisive-blunder swing: **2586 cp as White**, **2346 cp as Black** (13 vs 14 blundered games).

Read this against the eval_v2 phase numbers (`docs/policy_v2_eval_100k.md`):
- quiet rank_gain — opening +3.99, middlegame +3.61, **endgame +0.96** (much weaker)
- baseline-rank — `1` and `2` slightly demoted, `6..10` and `11+` strongly promoted

If most of the candidate's losses cluster in **endgame** or **middlegame quiet** decisions, that's consistent with the eval-side weakness. Opening losses would be more surprising and would suggest the policy is corrupting opening tactical choices that baseline gets right with MVV-LVA + history alone.

## 5. One-line recommendation

**Endgame-dominant losses.** Matches the eval prediction — the model is weak in endgames and that translates to game losses.

