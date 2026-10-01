# BLAKE2s reference implementation (vendored)

Used for command signatures (DS-52): keyed BLAKE2s, a 32-byte tag.

| | |
|---|---|
| Source | https://github.com/BLAKE2/BLAKE2, directory `ref/` |
| Commit | `ed1974ea83433eba7b2d95c5dcd9ac33cb847913` |
| Files | `blake2s-ref.c`, `blake2.h`, `blake2-impl.h`, `COPYING` |
| Licence | CC0 1.0, OpenSSL, or Apache 2.0, at our option (see the file headers) |

The files are copied unchanged. Do not edit them here; to update, copy the
same files from a newer commit and change the commit above.

Zephyr's crypto stack does not provide BLAKE2s, and Monocypher implements
only BLAKE2b, so the reference code is the simplest source.
