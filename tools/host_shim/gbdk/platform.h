/*
 * Host-build shim for GBDK's <gbdk/platform.h>.
 *
 * The depth-plane coefficient tables in build/generated/polar_depthplane are
 * GENERATED for the Game Gear target and carry GBDK banking annotations. The
 * census and oracle tools compile that same generated source natively so the
 * host check runs against the bytes the target will use, rather than a
 * re-implementation.
 *
 * On the host these annotations have no meaning:
 *   BANKED / NONBANKED  - calling-convention storage qualifiers
 *   BANKREF / BANKREF_EXTERN / BANK - declare and reference linker bank symbols
 *
 * Erasing them is therefore semantically safe for a host build and changes
 * nothing about the data. Target builds never see this file: they use the real
 * GBDK header from the toolchain include path.
 *
 * Without this, `make target-solve-census` fails to compile outright - first on
 * a missing header, then on BANKREF() being parsed as a K&R function
 * definition. That blocked reproduction of the 21,238 T depth-plane
 * column-solve measurement and the 99.10% path-split census.
 */
#ifndef GBDK_PLATFORM_HOST_SHIM_H
#define GBDK_PLATFORM_HOST_SHIM_H

#define BANKED
#define NONBANKED
#define BANKREF(name)
#define BANKREF_EXTERN(name)
#define BANK(name) 0
#define CRITICAL
#define INTERRUPT

#endif
