// ─────────────────────────────────────────────────────────────────────────────
// SANITIZE — the trust boundary for every string that arrives from a peer.
//
// EVERY inbound peer-supplied string (match name, player name, GRP label, chat)
// MUST pass through sanitizeText() BEFORE it reaches C. The C engine draws these
// through the Duke menu/HUD text path, which:
//   - interprets '^' as a palette / color escape (e.g. "^10" recolors following
//     text) — untrusted '^' lets a peer recolor or corrupt the menu, so strip it;
//   - is a fixed-size buffer world (MAXLISTNAMELEN == 32 in grpscan.h) — so cap
//     length well under that;
//   - is an 8-bit code page — so restrict to printable 7-bit ASCII (0x20–0x7E),
//     which every code page renders identically and which cannot smuggle control
//     bytes (NUL truncation, backspace, ESC, newline injection) into C strings.
//
// This module is pure and dependency-free so it is trivially unit-testable and can
// be reused by both the runtime and any tests.
// ─────────────────────────────────────────────────────────────────────────────

/** Hard cap on a sanitized string's length. One under MAXLISTNAMELEN (32) so the
 *  value plus a NUL terminator always fits the engine's name buffers. */
export const MAX_TEXT_LEN = 31;

/**
 * Reduce an arbitrary (untrusted) string to something safe to hand to the C
 * engine: printable ASCII only, no '^' palette escape, length-capped, trimmed.
 *
 * @param input   the untrusted string (any type is tolerated; non-strings -> "")
 * @param maxLen  optional shorter cap (defaults to MAX_TEXT_LEN)
 */
export function sanitizeText(input: unknown, maxLen: number = MAX_TEXT_LEN): string {
  if (typeof input !== "string") return "";
  let out = "";
  for (let i = 0; i < input.length && out.length < maxLen; i++) {
    const c = input.charCodeAt(i);
    if (c === 0x5e) continue; // '^' — Duke menu/HUD palette-escape; never allow from a peer
    if (c < 0x20 || c > 0x7e) continue; // drop control bytes and anything non-ASCII-printable
    out += input[i];
  }
  // Collapse to a trimmed value; a name that sanitizes to empty is caller's problem
  // (they substitute a default like "Player").
  return out.trim();
}

/**
 * Sanitize with a guaranteed non-empty result: if the input reduces to "", return
 * `fallback` (already assumed safe). Use for names that must render something.
 */
export function sanitizeName(input: unknown, fallback = "Player", maxLen: number = MAX_TEXT_LEN): string {
  const s = sanitizeText(input, maxLen);
  return s.length > 0 ? s : fallback;
}

/** True iff `s` is already fully sanitized (idempotence check, used in tests). */
export function isSanitized(s: string): boolean {
  return s === sanitizeText(s);
}
