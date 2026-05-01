# Global Active Task Count Non-Negative
**Source bug:** `36569780b0d64de283f9d6c2195fd1a43e221ee8`

No generic invariant applicable. Type-size overflow in a single field cast; the fix is reverting a type from `unsigned int` back to `unsigned long` and a cast from `(int)` to `(long)` — this is a one-off data-type regression with no reusable state predicate.
