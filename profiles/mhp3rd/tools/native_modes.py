"""Versioned native-helper names shared by local recording and delivery tools.

The B0 registration remains a frozen five-switch identity. A missing schema in
historical records and launch configurations means precisely those five switches.
"""

from __future__ import annotations


V2_SCHEMA = "yakumo-native-modes-v2"
LEGACY_FIELDS = (
    "MHP3RD_NATIVE_ANGLE_STEP",
    "MHP3RD_NATIVE_SCALE_MATRIX",
    "MHP3RD_NATIVE_TRANSLATION_MATRIX",
    "MHP3RD_NATIVE_VECTOR_CONSTRUCT",
    "MHP3RD_NATIVE_MATRIX_COPY",
)
VECTOR_BY_ENTRY = {
    0x08877244: "MHP3RD_NATIVE_VECTOR_NORM",
    0x08877264: "MHP3RD_NATIVE_VECTOR_NORM_SQUARED",
    0x08877280: "MHP3RD_NATIVE_VECTOR_DISTANCE",
    0x088772A8: "MHP3RD_NATIVE_VECTOR_DISTANCE_SQUARED",
}
VECTOR_FIELDS = tuple(VECTOR_BY_ENTRY.values())
V2_FIELDS = LEGACY_FIELDS + VECTOR_FIELDS
LEGACY_BY_ENTRY = {
    0x088775AC: "MHP3RD_NATIVE_ANGLE_STEP",
    0x08877818: "MHP3RD_NATIVE_VECTOR_CONSTRUCT",
    0x08878B28: "MHP3RD_NATIVE_SCALE_MATRIX",
    0x08878B4C: "MHP3RD_NATIVE_TRANSLATION_MATRIX",
    0x08879D08: "MHP3RD_NATIVE_MATRIX_COPY",
}
V2_BY_ENTRY = {**LEGACY_BY_ENTRY, **VECTOR_BY_ENTRY}


def fields(schema: str | None) -> tuple[str, ...]:
    """Resolve an explicitly supported schema; absence denotes legacy v1."""
    if schema is None:
        return LEGACY_FIELDS
    if schema == V2_SCHEMA:
        return V2_FIELDS
    raise ValueError("Unknown native mode schema")


def entries(schema: str | None) -> frozenset[int]:
    fields(schema)
    return frozenset(LEGACY_BY_ENTRY if schema is None else V2_BY_ENTRY)


def extra_fields(value: dict, schema: str | None) -> set[str]:
    """Find every undeclared native switch, including unknown future names."""
    declared = set(fields(schema))
    return {key for key in value if key.startswith("MHP3RD_NATIVE_") and key not in declared}
