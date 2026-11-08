#!/usr/bin/env python3
"""Tier/model configuration for the Phase-2 texture upscale pipeline.

Purpose: single source of truth mapping texture relpaths (relative to
Extracted/converted/textures/) to an upscale tier, and each tier to its
target resolution + Real-ESRGAN model. Imported by upscale_textures.py
and make_contact_sheets.py; not a CLI.

Input : relpath strings (any slash style, case-insensitive)
Output: tier name via tier_for(); settings via TIERS / ALPHA_MODE
Deps  : Python 3.8+ stdlib only
"""

import hashlib

# Ordered rules — FIRST match wins. Order matters:
#   1. TFA2/FONTS must beat the generic ui rules (fonts get their own knob).
#   2. USERINTERFACE dirs appear under several parents (TFA/, TFA2/,
#      TA_XTRAS/), so ui must match before hero/env would swallow them.
# Rule kinds: "prefix" tests relpath.startswith(pat);
#             "contains" tests pat in "/" + relpath.
# Coverage regression reference (2026-07-02, 1,793 files, 0 unmatched):
#   hero 325 / env 984 / props 126 / ui+ui_font 358
RULES = [
    ("ui_font", "prefix", "TFA2/FONTS/"),
    ("ui", "contains", "/USERINTERFACE/"),
    ("ui", "prefix", "TFA2/HUD/"),
    ("hero", "prefix", "TFA/ANIMATION/"),
    ("hero", "prefix", "TFA2/ANIMATION/"),
    ("env", "prefix", "TFA/LEVELS/"),
    ("env", "prefix", "TFA2/SKY/"),
    ("env", "prefix", "TFA2/REFLECTIONS/"),
    ("env", "prefix", "TFA2/ECOTEXTURES/"),
    ("props", "prefix", "TFA/PICKUPS/"),
    ("props", "prefix", "TFA2/FXTXTR/"),
]

# Per-tier settings.
#   target_long: desired long-edge of the output (never exceeded; sources
#     too small to reach it after passes_cap chained 4x passes land short).
#   model: realesrgan-ncnn-vulkan model name, or a "pil-*" pseudo-model
#     (pil-lanczos / pil-nearest) handled entirely in Pillow — the escape
#     hatch for content AI models mangle.
#   passes_cap: max chained 4x passes (2 => 16x max — beyond that the
#     model hallucinates mush on PS2-resolution sources).
#   small_max/small_model: sources with long edge <= small_max use
#     small_model instead — pilot showed x4plus invents structure
#     (letterform-like shapes, merged UI bars) on <=32px inputs.
# Models chosen from the 2026-07-02 M2 pilot sheets
# (Extracted/review/pilot/sheets): x4plus is photographic and faithful on
# every tier at >=64px; x4plus-anime cel-shades world textures (rejected);
# animevideov3 is soft but never invents structure (used for tiny world
# textures); pil-lanczos keeps tiny geometric UI elements faithful. Font
# atlases upscaled cleanly with x4plus (no cross-cell bleed), so ui_font
# kept the AI path with a single pass.
TIERS = {
    "hero":    {"target_long": 4096, "model": "realesrgan-x4plus", "passes_cap": 2,
                "small_max": 32, "small_model": "realesr-animevideov3"},
    "env":     {"target_long": 2048, "model": "realesrgan-x4plus", "passes_cap": 2,
                "small_max": 32, "small_model": "realesr-animevideov3"},
    "props":   {"target_long": 1024, "model": "realesrgan-x4plus", "passes_cap": 2,
                "small_max": 32, "small_model": "realesr-animevideov3"},
    "ui":      {"target_long": 1024, "model": "realesrgan-x4plus", "passes_cap": 2,
                "small_max": 32, "small_model": "pil-lanczos"},
    "ui_font": {"target_long": 1024, "model": "realesrgan-x4plus", "passes_cap": 1},
}

# How non-opaque textures go through the AI model:
#   "direct" — feed RGBA straight to the binary (requires it to preserve
#     alpha; verified/decided by the M2 pilot).
#   "split"  — upscale RGB and alpha (as 3-channel gray) separately through
#     the same pass chain, recombine at finalize.
# Fully-opaque textures never depend on this: they are always stripped to
# RGB before the binary and get constant-255 alpha re-attached.
# 2026-07-02 pilot: direct confirmed — the ncnn binary preserved alpha on
# all 35 non-opaque samples (0 class assertions failed, edges clean).
ALPHA_MODE = "direct"

# hero > env > props > ui > ui_font: when byte-identical duplicates of one
# texture live in folders of different tiers, the highest tier wins.
TIER_RANK = {"hero": 0, "env": 1, "props": 2, "ui": 3, "ui_font": 4}


def tier_for(relpath):
    """Map a texture relpath to its tier. Raises ValueError on no match so
    unclassified files fail loudly instead of silently skipping."""
    r = relpath.replace("\\", "/").upper()
    probe = "/" + r
    for tier, kind, pat in RULES:
        if kind == "prefix" and r.startswith(pat):
            return tier
        if kind == "contains" and pat in probe:
            return tier
    raise ValueError(f"no tier rule matches: {relpath}")


def settings_hash():
    """Short hash of the effective config; stamped into every manifest
    record so config changes invalidate previously-upscaled files."""
    blob = repr((RULES, sorted(TIERS.items()), ALPHA_MODE)).encode()
    return hashlib.sha1(blob).hexdigest()[:12]
