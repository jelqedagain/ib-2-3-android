// Player names (shown on the public page and the admin page's leaderboards): 3 to 16 letters, digits and spaces,
// one per player, no two alike (ignoring case and spaces), and none of the blocked words. The words are compared
// after undoing the usual disguises (case, spaces and symbols, 0 for o, 4 for a, $ for s, letters repeated), and
// are kept base64-encoded so this file does not spell them out.
const LIST = JSON.parse(atob("eyJ3b3JkcyI6IFsibmlnZ2VyIiwgIm5pZ2dhIiwgIm5pZ2VyIiwgIm5lZ3JvIiwgIm5pZ25vZyIsICJjb29uIiwgInNwaWMiLCAic3BpY2siLCAid2V0YmFjayIsICJiZWFuZXIiLCAiY2hpbmsiLCAiZ29vayIsICJ6aXBwZXJoZWFkIiwgImtpa2UiLCAia3lrZSIsICJoZWViIiwgImh5bWllIiwgInJhZ2hlYWQiLCAidG93ZWxoZWFkIiwgInNhbmRuaWdnZXIiLCAicGFraSIsICJ3b2ciLCAiZ29sbGl3b2ciLCAiamlnYWJvbyIsICJwb3JjaG1vbmtleSIsICJqdW5nbGVidW5ueSIsICJ0YXJiYWJ5IiwgInJlZHNraW4iLCAiaW5qdW4iLCAic3F1YXciLCAiZ3lwcG8iLCAiZ3lwbyIsICJwaWtleSIsICJmYWdnb3QiLCAiZmFnb3QiLCAiZmFnIiwgImR5a2UiLCAidHJhbm55IiwgInNoZW1hbGUiLCAicmV0YXJkIiwgInJldGFyZGVkIiwgInNwYXN0aWMiLCAic3BheiIsICJoaXRsZXIiLCAibmF6aSIsICJoZWlsaGl0bGVyIiwgInNpZWdoZWlsIiwgImtrayIsICJ3aGl0ZXBvd2VyIiwgIjE0ODgiLCAiZ2FzdGhlamV3cyIsICJraWxsamV3cyIsICJyYXBlIiwgInJhcGlzdCIsICJwZWRvIiwgInBhZWRvIiwgInBlZG9waGlsZSIsICJtb2xlc3QiLCAiY3VudCIsICJ3aG9yZSIsICJzbHV0IiwgImN1bSIsICJqaXp6IiwgInBlbmlzIiwgInZhZ2luYSIsICJwdXNzeSIsICJkaWNrIiwgImNvY2siLCAicG9ybiIsICJhbmFsIiwgImFudXMiLCAiZGlsZG8iLCAiYmxvd2pvYiIsICJoYW5kam9iIiwgImFkbWluIiwgImFkbWluaXN0cmF0b3IiLCAibW9kZXJhdG9yIiwgImNsYXNobW9iIiwgInNlcnZlciIsICJvZmZpY2lhbCIsICJzdGFmZiJdLCAid2hvbGUiOiBbImZhZyIsICJjdW0iLCAiYW5hbCIsICJhbnVzIiwgImRpY2siLCAiY29jayIsICJjb29uIiwgInNwaWMiLCAid29nIiwgImtrayIsICJwYWtpIiwgImhlZWIiLCAicmFwZSIsICJzdGFmZiJdLCAiYWxsb3dlZCI6IFsic2N1bnRob3JwZSIsICJ0aGVyYXBpc3QiLCAidGhlcmFwaXN0cyIsICJ0b3JwZWRvIiwgInNwZWVkbyIsICJwZWRvbWV0ZXIiLCAibmlnZXJpYSIsICJuaWdlcmlhbiIsICJwZW5pc3RvbmUiLCAiY29ja3RhaWwiLCAiaGFuY29jayIsICJwZWFjb2NrIiwgImRpY2tlbnMiLCAiYW5hbHlzdCIsICJjbGFzc2ljIl19"));

// "N 1 g g 3 r" -> "niger": lower case, look-alike digits and symbols as letters, only letters, no repeats
export function squash(s) {
  const look = { 0: "o", 1: "i", 3: "e", 4: "a", 5: "s", 7: "t", 8: "b", 9: "g", "$": "s", "@": "a", "!": "i", "|": "i" };
  return s.toLowerCase().replace(/[0-9$@!|]/g, (c) => look[c] || c).replace(/[^a-z]/g, "").replace(/(.)\1+/g, "$1");
}

const WORDS = LIST.words.map(squash), WHOLE = new Set(LIST.whole.map(squash)), ALLOWED = LIST.allowed;

// The name tidied up (spaces trimmed and single), or an error message.
export function checkName(raw) {
  const name = String(raw || "").trim().replace(/\s+/g, " ");
  if (name.length < 3 || name.length > 16) return { error: "Names are 3 to 16 characters." };
  if (!/^[A-Za-z0-9 ]+$/.test(name)) return { error: "Names can only have letters, numbers and spaces." };
  if (/^player [0-9a-f]+$/i.test(name)) return { error: "That name is not allowed." };
  // ordinary words that contain a blocked one are taken out first ("Therapist"), then: a blocked word anywhere in
  // the name run together, or a short one as a word of its own ("xX fag Xx", not "Fagin")
  let flat = squash(name), digits = name.toLowerCase().replace(/[^a-z0-9]/g, "");
  for (const a of ALLOWED) {
    flat = flat.split(squash(a)).join("_");
    digits = digits.split(a).join("_");
  }
  const words = name.split(" ").map(squash);
  if (WHOLE.has(flat) || words.some((w) => WHOLE.has(w)) ||
      WORDS.some((w) => !WHOLE.has(w) && (flat.includes(w) || digits.includes(w))))
    return { error: "That name is not allowed." };
  return { name };
}

// What two names are compared by, to keep them unique ("Joel", "joel" and "J O E L" are the same name)
export const nameKey = (name) => name.toLowerCase().replace(/ /g, "");
