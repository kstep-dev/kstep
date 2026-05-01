# Relax Domain Level Off-by-One
**Source bug:** `a1fd0b9d751f840df23ef0e75b691fc00cfd4743`

No generic invariant applicable. Off-by-one in a comparison operator (`>` vs `>=`) specific to the `relax_domain_level` feature's parameter-to-flag mapping — too narrow to generalize beyond this single code path.
