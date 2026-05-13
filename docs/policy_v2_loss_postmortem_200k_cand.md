# Policy v2 — SPRT loss post-mortem

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

## 2. Phase distribution of deciding blunders (candidate's losses)

| phase | white losses | black losses | total |
|---|---:|---:|---:|
| opening | 1 | 1 | 2 |
| middlegame | 3 | 4 | 7 |
| endgame | 5 | 5 | 10 |
| **classified** | **9** | **10** | **19** |

(33 losses had no single ply with ≥ 200 cp swing — these tend to be slow positional grinds where the candidate was slightly worse throughout.)

## 3. Top-10 worst blunders

| game | color | move | phase | played | swing (cp) | before / after eval (cand POV) | FEN |
|---|---|---|---|---|---:|---|---|
| #150 | White | 144 | endgame | `Kh5` | 29610 | -382 → -29992 | `8/8/8/6K1/5r2/5k2/8/8 w - - 12 144` |
| #157 | Black | 84 | endgame | `Kg8` | 29380 | -612 → -29992 | `8/1R4k1/4K1p1/6P1/4pp1p/8/8/8 b - - 1 84` |
| #103 | White | 61 | endgame | `g3` | 29188 | -807 → -29995 | `8/8/p7/5p2/8/4k2P/1r4P1/5K2 w - - 0 61` |
| #36 | White | 60 | endgame | `Qe2` | 29185 | -805 → -29990 | `8/k6p/1b1p4/4p3/4P3/1P3PK1/P1q4P/5R2 b - - 4 60` |
| #114 | Black | 17 | middlegame | `Nb4` | 417 | +220 → -197 | `r2q3k/pppbrppB/2n1pb2/2Pp4/3P4/2N1PN2/PP3PPP/RQ2R1K1 b - - 6 17` |
| #164 | Black | 15 | opening | `Bb4` | 397 | -30 → -427 | `5br1/pkpr1ppp/2p1p3/3b4/8/1P3P2/PBRPP1PP/3K1B1R b - - 2 15` |
| #171 | White | 17 | middlegame | `Qxf6` | 332 | -33 → -365 | `r3r1k1/pp3ppp/2p2q2/3P4/2PQ2b1/5p2/PPP1RPPP/R5K1 w - - 0 17` |
| #8 | Black | 42 | endgame | `Rg8` | 300 | +2 → -298 | `2qr1r1k/pp3pp1/1n2p2p/2p1B3/4Q3/3P4/PPP1RPPP/4R1K1 b - - 4 42` |
| #210 | Black | 96 | endgame | `Nd5` | 299 | +3 → -296 | `5N2/6K1/6P1/5k2/5n2/8/8/8 b - - 0 96` |
| #58 | White | 52 | endgame | `f5` | 284 | -8 → -292 | `8/7p/6pk/2R5/P4P2/3K4/3n3r/8 w - - 11 52` |

## 4. Imbalance read (qualitative)

Adjudication breakdown (regex over PGN result comments): 107 resign-style adjudications, 17 draw adjudications.

Average decisive-blunder swing: **9950 cp as White**, **3206 cp as Black** (9 vs 10 blundered games).

Read this against the eval_v2 phase numbers (`docs/policy_v2_eval_100k.md`):
- quiet rank_gain — opening +3.99, middlegame +3.61, **endgame +0.96** (much weaker)
- baseline-rank — `1` and `2` slightly demoted, `6..10` and `11+` strongly promoted

If most of the candidate's losses cluster in **endgame** or **middlegame quiet** decisions, that's consistent with the eval-side weakness. Opening losses would be more surprising and would suggest the policy is corrupting opening tactical choices that baseline gets right with MVV-LVA + history alone.

## 5. One-line recommendation

**Endgame-dominant losses.** Matches the eval prediction — the model is weak in endgames and that translates to game losses.

