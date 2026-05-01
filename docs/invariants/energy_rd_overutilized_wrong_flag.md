# rd->overutilized Wrong Flag
**Source bug:** `cd18bec668bb6221a54f03d0b645b7aed841f825`

No generic invariant applicable. Wrong-variable typo (`sg_overloaded` passed instead of `sg_overutilized`); any invariant would merely restate the intended code logic for this single call site.
