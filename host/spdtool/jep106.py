"""JEP-106 manufacturer names for the IDs that commonly appear on DIMMs.

SPD stores a manufacturer as two bytes: the number of 0x7F continuation codes
(bank - 1) with an odd-parity bit 7, then the ID byte itself (which carries
its own parity bit). This is a small subset of JEP-106; unknown IDs are shown
as bank/code.
"""

from __future__ import annotations

# (bank, id byte including parity) -> name
_NAMES = {
    (1, 0x2C): "Micron",
    (1, 0x98): "Toshiba",
    (1, 0xAD): "SK hynix",
    (1, 0xC1): "Infineon",
    (1, 0xCE): "Samsung",
    (2, 0x98): "Kingston",
    (3, 0x9E): "Corsair",
    (4, 0x0B): "Nanya",
    (5, 0xCB): "ADATA",
    (5, 0xCD): "G.Skill",
    (5, 0xEF): "Team Group",
    (6, 0x9B): "Crucial",
}


def name(cont: int, code: int) -> str:
    if cont == 0 and code == 0:
        return "not specified"
    bank = (cont & 0x7F) + 1
    n = _NAMES.get((bank, code))
    return n if n else "unknown (bank %d, 0x%02x)" % (bank, code)
