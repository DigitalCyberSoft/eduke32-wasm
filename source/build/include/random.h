// "Build Engine & Tools" Copyright (c) 1993-1997 Ken Silverman
// Ken Silverman's official web site: "http://www.advsys.net/ken"
// See the included license file "BUILDLIC.TXT" for license info.
//
// This file has been modified from Ken Silverman's original release.

/* Routines for generation of pseudo-random integer sequences. */

#pragma once

#ifndef random_h
#define random_h

#include "compat.h"

#define WRAND_MAX 32767u

#ifdef engine_c_
int32_t randomseed;
uint32_t wrandomseed = 1;
int32_t g_krandCalls;
#if defined(NETDUKE32) && !KRANDDEBUG
const char *g_krandSiteRing[8];
int32_t krand_traced(const char *site)
{
    if (g_krandCalls < 8)
        g_krandSiteRing[g_krandCalls] = site;
    g_krandCalls++;
    randomseed = (randomseed * 1664525ul) + 221297ul;
    return ((uint32_t)randomseed) >> 16;
}
#endif
#else
extern int32_t randomseed;
extern uint32_t wrandomseed;
// Sim-RNG draw counter (NetDuke32 desync forensics): every krand() advances
// randomseed, so the per-tic CALL COUNT is itself lockstep state -- a count
// mismatch at a tic names "extra RNG consumer" directly, and a count that
// flakes between stamps exposes frame-rate callers polluting the sim stream.
extern int32_t g_krandCalls;
#if defined(NETDUKE32) && !KRANDDEBUG
extern const char *g_krandSiteRing[8];   // this tic's first call sites
#endif
#endif

#if defined(NETDUKE32) && !KRANDDEBUG
// RNG TRACE (residual-fork forensics): every call site self-tags with a static
// FILE:LINE string; sync.cpp keeps the current tic's first sites and dumps them
// when the per-tic draw-count category (16) mismatches -- diffing the two
// peers' dumps names the asymmetric consumer directly. Same LCG, out of line
// (defined in engine.cpp via the engine_c_ block above, so every binary that
// links the engine -- game and tools alike -- resolves it).
int32_t krand_traced(const char *site);
#define E32_KR_STR2(x) #x
#define E32_KR_STR(x) E32_KR_STR2(x)
#define krand() krand_traced(__FILE__ ":" E32_KR_STR(__LINE__))
#elif !KRANDDEBUG
static FORCE_INLINE int32_t krand(void)
{
    g_krandCalls++;
    randomseed = (randomseed * 1664525ul) + 221297ul;
    return ((uint32_t) randomseed)>>16;
}
#else
int32_t    krand(void);
#endif

static FORCE_INLINE int32_t seed_krand(int32_t* seed)
{
    *seed = (*seed * 1664525ul) + 221297ul;
    return ((uint32_t)*seed) >> 16;
}

// This aims to mimic Watcom C's implementation of rand
static FORCE_INLINE int32_t wrand(void)
{
    wrandomseed = 1103515245 * wrandomseed + 12345;
    return (wrandomseed >> 16) & 0x7FFF;
}

static FORCE_INLINE void wsrand(int seed)
{
    wrandomseed = seed;
}

#endif
