"""Policy v2 dataset schema constants.

The v2 format is a single .npz (or directory of standalone .npy arrays)
with the fields below. Compared to v1:

  - schema_version is recorded explicitly so future bumps are obvious.
  - per-position context is stored once in `ctx_features`; per-move
    inputs are in `move_features`. v1 replicated the context into every
    move row.
  - `baseline_scores` records the engine's pre-policy ordering score for
    each candidate. Required for fused offline metrics (see eval_v2.py).
  - `move_buckets` is per-move (quiet / capture / check / promotion bit
    flags), not per-position-of-target as in v1.
  - `teacher_scores` carries a per-move soft target (NaN where the
    teacher did not score that move). Combined with the proposal mask
    this supports both all-legal and proposal-set supervision.

Files (npz keys, or filenames in directory mode):

  schema_version : int32 scalar              = 2
  group_offsets  : int64[N+1]                offsets into per-move arrays
  move_features  : float32[Σ Lᵢ, F_move]
  ctx_features   : float32[N,    F_ctx]
  baseline_scores: float32[Σ Lᵢ]             pre-policy ordering score
  move_buckets   : uint8[Σ Lᵢ]               bit flags 1=quiet 2=cap 4=check 8=promo
  teacher_scores : float32[Σ Lᵢ]             per-move soft target (NaN if absent)
  proposal_mask  : uint8[Σ Lᵢ]               1 if move is in the proposal set
  target_indices : int64[N]                  legacy bestmove index for fallback
  bucket_flags   : uint8[N]                  bucket of the teacher move (legacy)
  meta           : utf-8 json scalar          {teacher_mode, proposal_mode, ...}

Sidecar text files (in directory mode or alongside the .npz):

  fens.txt        : one FEN per group, in group order
  bestmoves.txt   : one UCI bestmove per group (target)
"""

from __future__ import annotations

SCHEMA_VERSION = 2

# Bucket bit flags. Must match Bucket enum in src/policy.cpp.
BUCKET_QUIET    = 1 << 0
BUCKET_CAPTURE  = 1 << 1
BUCKET_CHECK    = 1 << 2
BUCKET_PROMO    = 1 << 3

BUCKET_ID_QUIET   = 0
BUCKET_ID_CAPTURE = 1
BUCKET_ID_CHECK   = 2
BUCKET_ID_PROMO   = 3

BUCKET_NAMES = ("quiet", "capture", "check", "promotion")

TEACHER_MODES = (
    "stockfish_multipv",
    "rinnegan_deep_multipv",
    "hard_bestmove_fallback",
)

PROPOSAL_MODES = ("all_legal", "proposal_set")

NPZ_KEYS = (
    "schema_version",
    "group_offsets",
    "move_features",
    "ctx_features",
    "baseline_scores",
    "move_buckets",
    "teacher_scores",
    "proposal_mask",
    "target_indices",
    "bucket_flags",
    "meta",
)


def bucket_flags_for(is_quiet: bool, is_capture: bool, gives_check: bool, is_promo: bool) -> int:
    flags = 0
    if is_quiet:    flags |= BUCKET_QUIET
    if is_capture:  flags |= BUCKET_CAPTURE
    if gives_check: flags |= BUCKET_CHECK
    if is_promo:    flags |= BUCKET_PROMO
    return flags


def primary_bucket(flags: int) -> int:
    """Map bucket bit-flags to a single bucket id (matches engine ordering).

    Promotion takes precedence over capture, capture over check, check over
    quiet — same priority as `scoreMoveV2Raw` in src/policy.cpp.
    """
    if flags & BUCKET_PROMO:   return BUCKET_ID_PROMO
    if flags & BUCKET_CAPTURE: return BUCKET_ID_CAPTURE
    if flags & BUCKET_CHECK:   return BUCKET_ID_CHECK
    return BUCKET_ID_QUIET
