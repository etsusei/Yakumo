"""Supplemental renderer texture policy, separate from PSP native helpers.

Historical launch configurations and journals omit these fields. Their
effective texture mode is off, without changing their recorded identity.
"""

from __future__ import annotations

from typing import Any


SCHEMA = "yakumo-texture-decode-v1"
ENVIRONMENT = "MHP3RD_PORTABLE_TEXTURE_DECODE"
SCHEMA_FIELD = "texture_decode_schema"
MODE_FIELD = "texture_decode_mode"
FIELDS = frozenset({SCHEMA_FIELD, MODE_FIELD})
MODES = frozenset({"off", "verify", "native"})


def mode(value: dict[str, Any]) -> str:
    """Validate the optional complete field pair and return its effective mode."""
    present = FIELDS & value.keys()
    if not present:
        return "off"
    if present != FIELDS:
        raise ValueError("texture decode policy fields must appear together")
    if value[SCHEMA_FIELD] != SCHEMA:
        raise ValueError("unknown texture decode schema")
    selected = value[MODE_FIELD]
    if type(selected) is not str or selected not in MODES:
        raise ValueError("invalid texture decode mode")
    return selected
