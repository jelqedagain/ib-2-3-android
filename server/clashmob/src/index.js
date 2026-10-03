// ClashMob server for the IB3 Android port: a Cloudflare Worker with a D1 (SQLite) database.
//
// It answers the game's ClashMob requests (McpClashMobManagerV3, /sword/api/challenges...), which the port forwards
// here as they are (src/game/clashmob.cpp, "the community server"). It runs IB3's ClashMob 2.0, with its three kinds
// of event (events.js has the format):
//   Trial        solo (challengeType SOLO): each play is scored, the best one earns Bronze / Silver / Gold rewards
//   ClashMob     co-op (SOCIAL, gated on SUCCESS): everyone plays toward one goal. With stages, the mob must reach
//                one stage's goal to open the next; each stage's reward goes to everyone who played it once the mob
//                clears it, and the whole event has to be cleared before its time runs out
//   Tournament   an Aegis Tournament (SOCIAL, gated on TOP_N_PERCENT): timed stages; each stage ranks the players'
//                best scores, and only the top TopPercent percent go on to the next stage
// A staged event is a parent challenge listing its stages (childChallengeList); the stages are challenges of their own
// (parentChallengeId), and the parent names the stage being played (activeChildChallengeId). The game builds its
// screens from that (SwordClashMobManager.QueryChallengeListComplete, SwordClashMobData.Update).
//
// Players: the game has no online account (see clashmob.cpp), so the port sends its own player id and secret key in
// X-ClashMob-Player / X-ClashMob-Key. The first request with an id ties it to that key; later ones must match.
//
// Pages: / shows the live events to anyone (from /status); /admin is the admin page (public.html, admin.html). The admin
// API (/admin/api/...) needs the ADMIN_KEY secret as the password ("Authorization: Bearer <password>"). For tests only,
// TIME_TRAVEL=1 lets requests set the server's clock (X-ClashMob-Now).
//
// Abuse limits (see "connections"): 100 requests a minute and NEW_PLAYERS_PER_DAY new players a day per connection,
// 10 wrong admin passwords an hour per connection, and per player: one scoring fight every MIN_PLAY_SECONDS.
import DEFAULT_EVENTS from "./events.js";
import { checkName, nameKey } from "./names.js";
import PUBLIC_PAGE from "./public.html";
import ADMIN_PAGE from "./admin.html";
import THEME_CSS from "./theme.css";

const DAY = 86400;
const GRACE = 600;  // a play that started before a stage or event ended still counts this long after

// What one play can add at most, by BattleType (MaxScore= overrides it).
function playCap(c) {
  if (c.maxScore > 0) return c.maxScore;
  switch (c.battleType) {
    case "BT_KillNBosses": return 1;
    case "BT_Kill1Boss": return c.bossHealth > 0 ? c.bossHealth : 10000000;
    case "BT_TimeSurvival": return Math.ceil(c.endTime) + 5;
    case "BT_TreasureCollection": return 200;
    default: return 500;  // BT_BattleChallengeTrigger
  }
}

// ---- the database ----

const SCHEMA = [
  `CREATE TABLE IF NOT EXISTS players (
     id TEXT PRIMARY KEY, key_hash TEXT NOT NULL, created INTEGER NOT NULL, seen INTEGER NOT NULL)`,
  `CREATE TABLE IF NOT EXISTS progress (
     challenge TEXT NOT NULL, player TEXT NOT NULL, slot TEXT NOT NULL DEFAULT '',
     attempts INTEGER NOT NULL DEFAULT 0, successful INTEGER NOT NULL DEFAULT 0,
     progress INTEGER NOT NULL DEFAULT 0, high INTEGER NOT NULL DEFAULT 0, award INTEGER NOT NULL DEFAULT 0,
     complete INTEGER NOT NULL DEFAULT 0, accept_time INTEGER NOT NULL DEFAULT 0,
     update_time INTEGER NOT NULL DEFAULT 0, last_play INTEGER NOT NULL DEFAULT 0,
     PRIMARY KEY (challenge, player))`,
  `CREATE TABLE IF NOT EXISTS won (challenge TEXT PRIMARY KEY, at INTEGER NOT NULL)`,
  `CREATE TABLE IF NOT EXISTS config (key TEXT PRIMARY KEY, value TEXT NOT NULL)`,
  // Each challenge's numbers, kept up to date as players join and play, so a request reads one row per challenge
  // instead of adding up every player's (D1's free plan counts every row read)
  `CREATE TABLE IF NOT EXISTS totals (
     challenge TEXT PRIMARY KEY, total INTEGER NOT NULL DEFAULT 0, players INTEGER NOT NULL DEFAULT 0,
     scorers INTEGER NOT NULL DEFAULT 0)`,
  // Ranks count the players above one: these keep that to an index range
  `CREATE INDEX IF NOT EXISTS progress_rank_total ON progress (challenge, complete, progress)`,
  `CREATE INDEX IF NOT EXISTS progress_rank_best ON progress (challenge, complete, high)`,
  // (ip: a connection's fingerprint, see connection(); never the address itself)
  `CREATE TABLE IF NOT EXISTS admin_failures (ip TEXT PRIMARY KEY, count INTEGER NOT NULL, since INTEGER NOT NULL)`,
  `CREATE TABLE IF NOT EXISTS registrations (ip TEXT NOT NULL, day INTEGER NOT NULL, count INTEGER NOT NULL, PRIMARY KEY (ip, day))`,
];
const SCHEMA_VERSION = "4";

let schemaReady = null;
function ensureSchema(db) {
  schemaReady ??= (async () => {
    await db.batch(SCHEMA.map((s) => db.prepare(s)));
    const v = await db.prepare("SELECT value FROM config WHERE key = 'schema'").first();
    if (v?.value !== SCHEMA_VERSION) {
      // player names (version 3): unique by nameKey
      const cols = (await db.prepare("PRAGMA table_info(players)").all()).results.map((c) => c.name);
      if (!cols.includes("name")) await db.prepare("ALTER TABLE players ADD COLUMN name TEXT").run();
      if (!cols.includes("name_key")) await db.prepare("ALTER TABLE players ADD COLUMN name_key TEXT").run();
      await db.prepare("CREATE UNIQUE INDEX IF NOT EXISTS players_name ON players (name_key) WHERE name_key IS NOT NULL").run();
      await db.prepare("DELETE FROM admin_failures").run();  // (version 4: kept by fingerprint, no longer by address)
      // totals for the progress made before they were kept (version 1)
      await db.batch([
        db.prepare(`INSERT OR REPLACE INTO totals (challenge, total, players, scorers)
                    SELECT challenge, SUM(progress), COUNT(*), SUM(successful > 0) FROM progress GROUP BY challenge`),
        db.prepare("INSERT OR REPLACE INTO config (key, value) VALUES ('schema', ?)").bind(SCHEMA_VERSION),
      ]);
    }
  })().catch((e) => {
    schemaReady = null;
    throw e;
  });
  return schemaReady;
}

// ---- the events ----

const SERVER_KEYS = ["Type", "Mode", "Days", "Hours", "Start", "Repeat", "StageHours", "Goal", "Score", "MaxScore", "TopPercent"];
const KINDS = { trial: "Trial", solo: "Trial", clashmob: "ClashMob", coop: "ClashMob", tournament: "Tournament", comp: "Tournament" };

function parseIni(text) {
  const sections = [];
  let cur = null;
  for (let line of text.split("\n")) {
    line = line.replace(/[\r ]+$/, "");
    if (!line || line[0] === ";") continue;
    const m = line.match(/^\[(.*)\]$/);
    if (m) {
      if (!/^[A-Za-z0-9_-]{1,40}(\.[0-9]{1,2})?$/.test(m[1])) throw new Error(`bad section name [${m[1]}]`);
      sections.push((cur = { name: m[1], keys: {}, lines: [] }));
      continue;
    }
    if (!cur) continue;
    const eq = line.indexOf("=");
    const key = eq < 0 ? line : line.slice(0, eq), value = eq < 0 ? "" : line.slice(eq + 1);
    if (SERVER_KEYS.includes(key)) cur.keys[key] = value;
    else cur.lines.push([key, line]);
  }
  return sections;
}

// A stage's event file lines: the event's, with the stage's keys in place of the same keys. A stage with any reward
// lines (".RewardType=" ...) replaces all of the event's.
function mergeLines(base, stage) {
  const keys = new Set(stage.map(([k]) => k)), dots = stage.some(([k]) => k[0] === ".");
  return [...base.filter(([k]) => !keys.has(k) && !(dots && k[0] === ".")), ...stage];
}

// One challenge from its settings and event file lines.
function challenge(fields, keys, lines) {
  const get = (k) => lines.filter(([key]) => key === k).map(([, l]) => l.slice(l.indexOf("=") + 1)).pop();
  const c = {
    ...fields,
    goal: Math.max(parseInt(keys.Goal) || 1, 1),
    total: keys.Score ? keys.Score.toLowerCase() !== "best" : fields.kind === "ClashMob",
    maxScore: parseInt(keys.MaxScore) || 0,
    topPercent: Math.min(Math.max(parseInt(keys.TopPercent) || 50, 1), 100),
    battleType: get("BattleType") || "",
    bossHealth: parseInt(get("BossHealth")) || 0,
    endTime: parseFloat(get("EndTime")) || 30,
    maxPlays: parseInt(get("MaxPlays")) || 0,
    file: fields.role === "parent" ? null : "[SwordBattleEvent]\nVersion=1.4\n" + lines.map(([, l]) => l + "\n").join(""),
  };
  c.cap = playCap(c);
  return c;
}

// All challenges of the events running at `now`: single events, and staged ones as a parent and its stages.
export function parseEvents(text, now) {
  const sections = parseIni(text);
  const out = [];
  for (const ev of sections.filter((s) => !s.name.includes("."))) {
    const k = ev.keys;
    const kind = KINDS[(k.Type || k.Mode || "ClashMob").toLowerCase()];
    if (!kind) throw new Error(`[${ev.name}] Type must be Trial, ClashMob or Tournament`);
    const hours = parseFloat(k.Hours) || (parseFloat(k.Days) || 7) * 24;
    const period = Math.max(Math.round(hours * 3600), 60);
    const stages = sections
      .filter((s) => s.name.startsWith(ev.name + "."))
      .sort((a, b) => parseInt(a.name.split(".")[1]) - parseInt(b.name.split(".")[1]));
    if (kind === "Trial" && stages.length) throw new Error(`[${ev.name}] a Trial has no stages`);
    // The event starts again every period, from Start= (a UTC date and time) or else from 1970-01-01. Before Start it
    // is shown as coming soon.
    const anchor = k.Start ? Date.parse(k.Start.endsWith("Z") ? k.Start : k.Start + "Z") / 1000 : 0;
    if (Number.isNaN(anchor)) throw new Error(`[${ev.name}] Start must be a date and time like 2026-10-03T18:00:00`);
    // Repeat=0: the event runs once, from Start, and is no longer listed a day after it ends
    const repeat = !/^(0|no|false|never)$/i.test(k.Repeat || "");
    if (!repeat && !k.Start) throw new Error(`[${ev.name}] an event that does not repeat needs a Start`);
    const start = !repeat || now < anchor ? anchor : anchor + Math.floor((now - anchor) / period) * period;
    // A tournament's stages last StageHours each (else they share the period), and it is over after the last one
    const stageLen = kind === "Tournament" && stages.length
      ? Math.min(Math.round((parseFloat(k.StageHours) || hours / stages.length) * 3600), Math.floor(period / stages.length)) : 0;
    const end = stageLen ? start + stageLen * stages.length : start + period;
    if (!repeat && now > end + DAY) continue;
    const id = `cm-${ev.name}-${Math.floor(start / 60)}`;
    if (!stages.length) {
      out.push(challenge({ id, name: ev.name, kind, role: "single", start, end, eventStart: start, eventEnd: end }, k, ev.lines));
      continue;
    }
    const parent = challenge({ id, name: ev.name, kind, role: "parent", start, end, eventStart: start, eventEnd: end }, k, ev.lines);
    parent.children = stages.map((s, i) => {
      const [cs, ce] = stageLen ? [start + i * stageLen, start + (i + 1) * stageLen] : [start, end];
      const c = challenge({ id: `${id}-s${i + 1}`, name: s.name, kind, role: "child", index: i, start: cs, end: ce, eventStart: start, eventEnd: end },
        { ...k, ...s.keys }, mergeLines(ev.lines, s.lines));
      c.parent = parent;
      return c;
    });
    out.push(parent, ...parent.children);
  }
  placePins(out);
  return out;
}

// The world map's ClashMob pins: three at each arena, named after it (the original game's ClashMob pins; no story
// quest uses them). A pin holds one quest and the story's quests take theirs first, so an event on a story pin
// (MapPin_Obelisk_A...) is hidden while a story mission is there. Each event shows at its arena's (SubMapName) pins:
// events at the same arena at the same time get the next one, in the order of the events file.
const ARENA_PINS = Object.fromEntries([
  ["cm_obelisk_art", "CM_Obelisk_Art2", "CM_Obelisk_Art3"],
  ["cm_lake_art", "CM_Lake_art2", "CM_Lake_art3"],
  ["cm_dunes_art", "CM_Dunes_Art2", "CM_Dunes_Art3"],
  ["C01_CM_Monastery_Art", "C01_CM_Monastery_Art2", "C01_CM_Monastery_Art3"],
  ["B20_CrackedDesert_CM", "B20_CrackedDesert_CM2", "B20_CrackedDesert_CM3"],
  ["B20_FieldBurning_CM", "B20_FieldBurning_CM2", "B20_FieldBurning_CM3"],
  ["B20_SandDay_CM", "B20_SandDay_CM2", "B20_SandDay_CM3"],
].map((pins) => [pins[0].toLowerCase(), pins]));

const fileValue = (file, key) => file?.match(new RegExp(`^${key}=(.*)$`, "m"))?.[1]?.trim() || "";

function placePins(all) {
  const placed = [];  // {arena, slot, start, end}
  for (const c of all.filter((c) => c.role !== "child")) {
    const files = c.children ? c.children.map((x) => x.file) : [c.file];
    const arena = fileValue(files[0], "SubMapName").toLowerCase(), pins = ARENA_PINS[arena];
    if (!pins) continue;  // (an arena the server does not know: its own QuestMapPin)
    const busy = placed.filter((o) => o.arena === arena && o.start < c.end && c.start < o.end).map((o) => o.slot);
    const slot = [0, 1, 2].find((i) => !busy.includes(i));
    if (slot === undefined) continue;  // a fourth event there at once: its own QuestMapPin (warnings() says so)
    placed.push({ arena, slot, start: c.start, end: c.end });
    for (const x of c.children || [c]) {
      x.file = /^QuestMapPin=/m.test(x.file)
        ? x.file.replace(/^QuestMapPin=.*$/m, `QuestMapPin=${pins[slot]}`)
        : x.file + `QuestMapPin=${pins[slot]}\n`;
    }
  }
}

async function eventsText(env) {
  const row = await env.DB.prepare("SELECT value FROM config WHERE key = 'events'").first();
  return row ? row.value : DEFAULT_EVENTS;
}

// ---- where each event stands ----

// The community's numbers for each challenge, by id.
async function community(env, all) {
  const out = {};
  for (const c of all) out[c.id] = { total: 0, players: 0, scorers: 0, wonAt: 0 };
  if (!all.length) return out;
  const marks = all.map(() => "?").join(","), ids = all.map((c) => c.id);
  const [sums, won] = await env.DB.batch([
    env.DB.prepare(`SELECT challenge, total, players, scorers FROM totals WHERE challenge IN (${marks})`).bind(...ids),
    env.DB.prepare(`SELECT challenge, at FROM won WHERE challenge IN (${marks})`).bind(...ids),
  ]);
  for (const r of sums.results) Object.assign(out[r.challenge], { total: r.total, players: r.players, scorers: r.scorers });
  for (const r of won.results) out[r.challenge].wonAt = r.at;
  return out;
}

// What the game is told about a challenge: whether it is over and won, and (for a parent) the stage being played.
function standing(c, com, now) {
  const n = com[c.id];
  if (c.role === "parent") {
    const kids = c.children.map((k) => standing(k, com, now));
    let active;
    if (c.kind === "Tournament") {
      active = Math.min(Math.max(Math.floor((now - c.start) / (c.children[0].end - c.children[0].start)), 0), kids.length - 1);
    } else {
      const open = kids.findIndex((k) => !k.won);  // the first stage not cleared yet
      active = open < 0 ? kids.length - 1 : open;
    }
    const last = kids[kids.length - 1];
    const over = now >= c.end, won = c.kind === "Tournament" ? over : last.won;
    return { active, activeId: c.children[active].id, won, completed: over || won, completedAt: won && c.kind !== "Tournament" ? last.completedAt : c.end,
      goal: c.children[active].goal, current: Math.min(com[c.children[active].id].total, c.children[active].goal), players: n.players };
  }
  const over = now >= c.end;
  if (c.kind === "ClashMob") {
    // A stage counts as reached once every stage before it is cleared.
    const before = c.role === "child" ? c.parent.children.slice(0, c.index) : [];
    const reached = before.every((k) => com[k.id].total >= k.goal);
    const won = reached && n.total >= c.goal;
    return { won, completed: won || over, completedAt: won ? n.wonAt || now : c.end, goal: c.goal, current: Math.min(n.total, c.goal), players: n.players, reached };
  }
  // Trials and tournament stages are over when their time is, and "won" (rewards for whoever qualified)
  return { won: over, completed: over, completedAt: c.end, goal: c.goal, current: n.total, players: n.players, reached: now >= c.start };
}

// ---- JSON, as the game reads it (McpClashMobManagerV3.ParseChallenge / ParseUserChallengeStatus) ----

const iso = (t) => (t ? new Date(t * 1000).toISOString() : "");

function hash(s) {
  let h = 0x811c9dc5;
  for (let i = 0; i < s.length; i++) h = Math.imul(h ^ s.charCodeAt(i), 0x01000193) >>> 0;
  return String(h);
}

function challengeJson(c, com, now) {
  const st = standing(c, com, now), n = com[c.id];
  return {
    challengeId: c.id,
    visibleDate: iso(c.eventStart - 3600),
    startDate: iso(c.start),
    endDate: iso(c.end),
    completedDate: st.completed ? iso(st.completedAt) : "",
    purgeDate: iso(c.eventEnd + 30 * DAY),
    challengeType: c.kind === "Trial" ? "SOLO" : "SOCIAL",
    attempts: st.players,  // the game shows it as "Mob size"
    successfulAttempts: n.scorers,
    goalValue: st.goal,
    goalStartValue: 0,
    goalCurrentValue: st.current,
    started: now >= c.start,
    visible: true,
    completed: st.completed,
    successful: st.won,
    facebookId: "",
    facebookLikes: 0,
    facebookComments: 0,
    facebookLikeScalar: 0,
    facebookCommentScalar: 0,
    facebookLikeGoalProgress: 0,
    facebookCommentGoalProgress: 0,
    twitterId: "",
    twitterRetweets: 0,
    twitterGoalProgress: 0,
    twitterRetweetsScalar: 0,
    parentChallengeId: c.role === "child" ? c.parent.id : "",
    activeChildChallengeId: c.role === "parent" ? st.activeId : "",
    childChallengeList: c.role === "parent" ? c.children.map((k, i) => ({ key: i, value: k.id })) : [],
    childChallengeGatingType: c.kind === "Tournament" ? "TOP_N_PERCENT" : "SUCCESS",
    childChallengeGatingValue: c.kind === "Tournament" ? 100 - c.topPercent : 0,
    challengeRatingType: c.total ? "TOTAL_PROGRESS" : "HIGH_PROGRESS",
    startedAt: iso(c.start),
    minChallengeDuration: 0,
    files: c.file ? [{
      // filename too is the event's own: games with a title file cache (the PC port) find a file's download, and
      // keep the file, by this name, so with one name for all they download the first event's file for every event
      filename: `${c.id}_BattleEvent_1.4.ib3`,
      uniqueFileName: `${c.id}_BattleEvent_1.4.ib3`,
      hash: hash(c.file),
      type: "ib3",
      shouldKeepPostChallenge: false,
    }] : [],
  };
}

// A player's status in a challenge. The game compares highGoalProgress with the reward tiers.
function statusJson(c, s, rank) {
  return {
    challengeId: c.id,
    epicId: "",  // the game's account id: none
    saveSlotId: s.slot,
    numAttempts: s.attempts,
    numSuccessfulAttempts: s.successful,
    goalProgress: s.progress,
    didComplete: !!s.complete,
    lastUpdateTime: iso(s.update_time),
    userAwardGiven: s.award,
    acceptTime: iso(s.accept_time),
    didPreregister: false,
    likedViaFacebook: false,
    commentedViaFacebook: false,
    retweeted: false,
    highGoalProgress: c.total ? s.progress : s.high,
    rank: rank.rank,
    percentRank: rank.percent,
  };
}

// ---- ranks ----

const scoreCol = (c) => (c.total ? "progress" : "high");

// A player's place among those who finished a play. Tournament stages tell the game percentRank against the gate
// (childChallengeGatingValue = 100 - TopPercent): at or above it exactly when the player is in the top TopPercent.
async function rankOf(env, c, s) {
  const col = scoreCol(c), score = c.total ? s.progress : s.high;
  const r = await env.DB.prepare(
    `SELECT (SELECT COUNT(*) FROM progress WHERE challenge = ?1 AND complete = 1 AND ${col} > ?2) AS above,
            (SELECT COUNT(*) FROM progress WHERE challenge = ?1 AND complete = 1) AS n`).bind(c.id, score).first();
  const rank = r.above + 1, n = Math.max(r.n, rank);
  let percent = Math.round((100 * (n - rank + 1)) / n);
  if (c.kind === "Tournament") {
    const gate = 100 - c.topPercent, qualified = s.complete && rank <= Math.ceil((n * c.topPercent) / 100);
    percent = qualified ? Math.max(percent, gate) : Math.min(percent, gate - 1);
  }
  return { rank, percent: Math.max(percent, 0) };
}

async function qualified(env, c, player) {
  const s = await getStatus(env, c, player);
  if (!s || !s.complete) return false;
  return (await rankOf(env, c, s)).percent >= 100 - c.topPercent;
}

const getStatus = (env, c, player) =>
  env.DB.prepare("SELECT * FROM progress WHERE challenge = ? AND player = ?").bind(c.id, player).first();

async function statusFor(env, c, player) {
  const s = await getStatus(env, c, player);
  return s ? statusJson(c, s, await rankOf(env, c, s)) : null;
}

// ---- joining ----

async function join(env, c, player, slot, now) {
  const r = await env.DB.prepare(
    "INSERT OR IGNORE INTO progress (challenge, player, slot, accept_time, update_time) VALUES (?1, ?2, ?3, ?4, ?4)")
    .bind(c.id, player, slot, now).run();
  if (r.meta.changes) {
    await env.DB.prepare(
      "INSERT INTO totals (challenge, players) VALUES (?1, 1) ON CONFLICT (challenge) DO UPDATE SET players = players + 1")
      .bind(c.id).run();
  } else {
    await env.DB.prepare("UPDATE progress SET slot = ?3 WHERE challenge = ?1 AND player = ?2 AND slot <> ?3")
      .bind(c.id, player, slot).run();
  }
}

// A player who joined a staged event is in the stage being played: in a ClashMob always (late joiners too), in a
// tournament's first stage always and in a later one only after qualifying in the stage before.
async function enroll(env, parent, player, com, now) {
  const p = await getStatus(env, parent, player);
  if (!p) return;
  const st = standing(parent, com, now), stage = parent.children[st.active];
  if (st.completed || (await getStatus(env, stage, player))) return;
  if (parent.kind === "Tournament" && stage.index > 0 && !(await qualified(env, parent.children[stage.index - 1], player))) return;
  await join(env, stage, player, p.slot, now);
}

// ---- requests ----

const json = (v, status = 200) =>
  new Response(JSON.stringify(v), { status, headers: { "Content-Type": "application/json" } });
const text = (v, status = 200, type = "text/plain; charset=utf-8") =>
  new Response(v, { status, headers: { "Content-Type": type } });

async function sha256(s) {
  const d = await crypto.subtle.digest("SHA-256", new TextEncoder().encode(s));
  return [...new Uint8Array(d)].map((b) => b.toString(16).padStart(2, "0")).join("");
}

// ---- connections ----
//
// Where the server counts something per connection (requests a minute, new players a day, wrong admin passwords), it
// uses a fingerprint of the connection's address: an HMAC with the IP_SALT secret, so the address itself is never
// kept and the fingerprint cannot be turned back into it.
async function connection(request, env) {
  const ip = request.headers.get("CF-Connecting-IP") || "local";
  const key = await crypto.subtle.importKey("raw", new TextEncoder().encode(env.IP_SALT || "clashmob"), { name: "HMAC", hash: "SHA-256" },
    false, ["sign"]);
  const mac = await crypto.subtle.sign("HMAC", key, new TextEncoder().encode(ip));
  return [...new Uint8Array(mac)].slice(0, 16).map((b) => b.toString(16).padStart(2, "0")).join("");
}

// New players a day from one connection (several phones can share an address, so it is not 1)
const newPlayersPerDay = (env) => parseInt(env.NEW_PLAYERS_PER_DAY ?? "10");

// The player making the request: their id, null if the request has none, or an error Response.
async function authenticate(request, env, now) {
  const id = request.headers.get("X-ClashMob-Player"), key = request.headers.get("X-ClashMob-Key");
  if (!id && !key) return null;
  if (!/^[A-Za-z0-9_-]{4,64}$/.test(id || "") || !/^[0-9a-f]{32,128}$/.test(key || ""))
    return json({ error: "bad player id or key" }, 400);
  const h = await sha256(key);
  const row = await env.DB.prepare("SELECT key_hash, seen FROM players WHERE id = ?").bind(id).first();
  if (!row) {
    // a new player: at most NEW_PLAYERS_PER_DAY from one connection (against made-up players)
    const ip = await connection(request, env), day = Math.floor(now / DAY);
    const used = await env.DB.prepare("SELECT count FROM registrations WHERE ip = ? AND day = ?").bind(ip, day).first();
    if (used && used.count >= newPlayersPerDay(env))
      return json({ error: "too many new players from this connection today" }, 429);
    await env.DB.batch([
      env.DB.prepare(`INSERT INTO registrations (ip, day, count) VALUES (?1, ?2, 1)
                      ON CONFLICT (ip, day) DO UPDATE SET count = count + 1`).bind(ip, day),
      env.DB.prepare("DELETE FROM registrations WHERE day < ?").bind(day - 1),
    ]);
    await env.DB.prepare("INSERT OR IGNORE INTO players (id, key_hash, created, seen) VALUES (?, ?, ?, ?)")
      .bind(id, h, now, now).run();
    const again = await env.DB.prepare("SELECT key_hash FROM players WHERE id = ?").bind(id).first();
    if (!again || again.key_hash !== h) return json({ error: "wrong key for this player" }, 403);
  } else if (row.key_hash !== h) {
    return json({ error: "wrong key for this player" }, 403);
  } else if (now - row.seen > 3600) {  // (at most hourly: D1's free plan also counts rows written)
    await env.DB.prepare("UPDATE players SET seen = ? WHERE id = ?").bind(now, id).run();
  }
  return id;
}

// One play's result. What looks impossible is not counted (the play still is): more than one play can score, a second
// scoring play within MIN_PLAY_SECONDS, plays beyond MaxPlays, plays outside the stage's or event's time.
async function updateProgress(env, c, player, q, com, now) {
  const s = await getStatus(env, c, player);
  const st = standing(c, com, now);
  let add = Math.max(parseInt(q.get("goalProgress") || "0") || 0, 0);
  const minGap = parseInt(env.MIN_PLAY_SECONDS ?? "10");
  const used = c.kind === "Trial" ? s.attempts : s.successful;
  const note = [];
  if (add > c.cap) note.push(`capped ${add} to ${c.cap}`), (add = c.cap);
  if (add > 0 && s.last_play && now - s.last_play < minGap) note.push("too soon"), (add = 0);
  if (add > 0 && c.maxPlays > 0 && used >= c.maxPlays) note.push("no plays left"), (add = 0);
  if (add > 0 && (now < c.start || now > c.end + GRACE)) note.push("outside its time"), (add = 0);
  if (add > 0 && c.kind === "ClashMob" && (!st.reached || (st.won && now > st.completedAt + GRACE))) note.push("stage not open"), (add = 0);
  if (note.length) console.log(`${player} ${c.id}: ${note.join(", ")}`);
  const done = (q.get("didComplete") || "").toLowerCase() === "true" ? 1 : 0;
  await env.DB.prepare(
    `UPDATE progress SET attempts = attempts + 1, successful = successful + ?3, progress = progress + ?4,
     high = MAX(high, ?4), complete = MAX(complete, ?5), update_time = ?6,
     last_play = CASE WHEN ?4 > 0 THEN ?6 ELSE last_play END WHERE challenge = ?1 AND player = ?2`)
    .bind(c.id, player, add > 0 ? 1 : 0, add, done, now).run();
  if (add > 0) {
    const t = await env.DB.prepare(
      `INSERT INTO totals (challenge, total, scorers) VALUES (?1, ?2, ?3)
       ON CONFLICT (challenge) DO UPDATE SET total = total + ?2, scorers = scorers + ?3 RETURNING total`)
      .bind(c.id, add, s.successful === 0 ? 1 : 0).first();
    if (c.kind === "ClashMob" && t.total >= c.goal)
      await env.DB.prepare("INSERT OR IGNORE INTO won (challenge, at) VALUES (?, ?)").bind(c.id, now).run();
  }
}

async function handleChallenges(request, env, p, url, now) {
  const player = await authenticate(request, env, now);
  if (player instanceof Response) return player;
  const all = parseEvents(await eventsText(env), now);
  const com = await community(env, all);
  const parents = all.filter((c) => c.role === "parent");

  // GET /sword/api/challenges: every challenge (stages too)
  if (p.length === 3) {
    if (player) for (const parent of parents) await enroll(env, parent, player, com, now);
    return json(all.map((c) => challengeJson(c, com, now)));
  }
  const c = all.find((x) => x.id === p[3]);
  if (!c) return json({}, 404);

  // GET .../{id}: one challenge; .../children: a staged event and its stages; .../file/{name}: a stage's event file
  if (p.length === 4) return json(challengeJson(c, com, now));
  if (p.length === 5 && p[4] === "children") return json([c, ...(c.children || [])].map((x) => challengeJson(x, com, now)));
  if (p.length === 6 && p[4] === "file") return c.file ? text(c.file, 200, "application/octet-stream") : json({}, 404);

  if (!player) return json({ error: "no player" }, 401);

  // POST .../{id}/users[?wantsParentChildInfo=true]: the statuses of the players listed in the body (the game has no
  // account ids, so: the player making the request), for a staged event its stages' too
  if (p.length === 5 && p[4] === "users") {
    if (c.role === "parent") await enroll(env, c, player, com, now);
    const out = [];
    for (const x of [c, ...(c.role === "parent" && url.searchParams.get("wantsParentChildInfo") !== "false" ? c.children : [])]) {
      const s = await statusFor(env, x, player);
      if (s) out.push(s);
    }
    return json(out);
  }

  // POST .../{id}/users/{epicId}/saveSlots/{slot}[/updateProgress|/updateReward]
  if (p.length >= 8 && p[4] === "users" && p[6] === "saveSlots") {
    const slot = p[7].slice(0, 32);
    if (c.role === "child" && c.kind === "Tournament" && c.index > 0 && !(await getStatus(env, c, player)) &&
        !(await qualified(env, c.parent.children[c.index - 1], player)))
      return json({ error: "did not qualify for this stage" }, 403);
    await join(env, c, player, slot, now);
    if (p.length === 8) {  // joined
      if (c.role === "parent") await enroll(env, c, player, com, now);
      return json(await statusFor(env, c, player));
    }
    if (p[8] === "updateProgress" && c.role !== "parent") {
      await updateProgress(env, c, player, url.searchParams, com, now);
      return json(await statusFor(env, c, player));
    }
    if (p[8] === "updateReward") {
      const award = Math.min(Math.max(parseInt(url.searchParams.get("rewardValue") || "0") || 0, 0), 1000);
      await env.DB.prepare("UPDATE progress SET award = ? WHERE challenge = ? AND player = ?").bind(award, c.id, player).run();
      return json(await statusFor(env, c, player));
    }
  }
  return json({}, 404);
}

// ---- player names (the app's launcher sets them: see names.js for the rules) ----

// Gives a player a name; "" clears it. An error message when the name is not allowed or someone else has it.
async function setName(env, player, raw) {
  if (raw === "" || raw === null) {
    await env.DB.prepare("UPDATE players SET name = NULL, name_key = NULL WHERE id = ?").bind(player).run();
    return { ok: true, name: "" };
  }
  const c = checkName(raw);
  if (c.error) return { ok: false, error: c.error };
  const key = nameKey(c.name);
  const other = await env.DB.prepare("SELECT id FROM players WHERE name_key = ? AND id <> ?").bind(key, player).first();
  if (other) return { ok: false, error: "That name is taken." };
  try {
    await env.DB.prepare("UPDATE players SET name = ?, name_key = ? WHERE id = ?").bind(c.name, key, player).run();
  } catch {
    return { ok: false, error: "That name is taken." };  // (taken at the same moment)
  }
  return { ok: true, name: c.name };
}

// GET /player: the player's name; POST /player/name with the name as the body: set it
async function handlePlayer(request, env, p, now) {
  const player = await authenticate(request, env, now);
  if (player instanceof Response) return player;
  if (!player) return json({ error: "no player" }, 401);
  if (p.length === 1 && request.method === "GET") {
    const r = await env.DB.prepare("SELECT name FROM players WHERE id = ?").bind(player).first();
    return json({ id: player, name: r?.name || "" });
  }
  if (p[1] === "name" && request.method === "POST") {
    const r = await setName(env, player, (await request.text()).slice(0, 100));
    return json(r, r.ok ? 200 : 400);
  }
  return json({}, 404);
}

// ---- the pages: the public one (/) and the admin one (/admin) ----

// What the pages show of each event (a staged event with its stages), and the best players.
async function overview(env, all, now, topCount) {
  const com = await community(env, all);
  const tops = all.filter((c) => c.role !== "parent").map((c) => env.DB.prepare(
    `SELECT g.player, p.name, g.${scoreCol(c)} AS score FROM progress g LEFT JOIN players p ON p.id = g.player
     WHERE g.challenge = ? AND g.${scoreCol(c)} > 0 ORDER BY g.${scoreCol(c)} DESC LIMIT ?`).bind(c.id, topCount));
  const results = tops.length ? await env.DB.batch(tops) : [];
  const top = {};
  all.filter((c) => c.role !== "parent").forEach((c, i) => (top[c.id] = results[i].results));
  const describe = (c) => {
    const st = standing(c, com, now), n = com[c.id];
    const line = (k) => (c.file || c.children?.[0]?.file || "").match(new RegExp(`^${k}=(.*)$`, "m"))?.[1] || "";
    const field = (k) => [...(c.file || "").matchAll(new RegExp(`^\\.${k}=(.*)$`, "gm"))].map((m) => m[1]);
    const goals = field("RewardGoal"), datas = field("RewardData");
    const rewards = field("RewardType").map((type, i) => ({ type, data: datas[i] || "", goal: parseFloat(goals[i]) || 0 }));
    return {
      id: c.id, name: c.name, kind: c.kind, title: line("Title"), desc: line("Desc"), boss: line("BossObj"),
      start: c.start, end: c.end, status: now < c.start ? "upcoming" : now >= c.end || st.completed ? "ended" : "live",
      won: st.won, goal: c.goal, total: n.total, players: n.players, scorers: n.scorers, score: c.total ? "Total" : "Best",
      rewards, top: top[c.id] || [],
      ...(c.role === "parent" ? { activeStage: st.active, stages: c.children.map(describe) } : {}),
    };
  };
  return all.filter((c) => c.role !== "child").map(describe);
}

// Problems with an events file that still parses: these do not stop a save, but the admin page shows them.
function warnings(all, previous) {
  const out = [];
  const top = all.filter((c) => c.role !== "child");
  const pins = {}, arenaPins = new Set(Object.values(ARENA_PINS).flat().map((p) => p.toLowerCase()));
  for (const c of top) {
    const file = c.file || c.children[0].file;
    const pin = file.match(/^QuestMapPin=(.*)$/m)?.[1];
    const overlap = (pins[pin] || []).find((o) => o.start < c.end && c.start < o.end);
    if (pin && overlap) out.push(`${c.name} and ${overlap.name} are on the same map pin (${pin}) at the same time: only one of them shows`);
    if (pin && !arenaPins.has(pin.toLowerCase()))
      out.push(`${c.name} is on a story map pin (${pin}): it is hidden while a story mission is there. ` +
        "Events at a ClashMob arena get the arena's own pins (three at each)");
    (pins[pin] ||= []).push(c);
    if (!file.match(/^BossObj=/m)) out.push(`${c.name} has no boss (BossObj)`);
    for (const x of c.children || [c]) {
      if (!x.file.match(/^\.RewardType=/m)) out.push(`${x.name} has no reward`);
      if (x.file.match(/^\.RewardType=TRA_(Item|Gem)_Fixed$/m) && x.file.match(/^\.RewardData=$/m))
        out.push(`${x.name}: an item or gem reward needs the item's name (RewardData)`);
      const types = [...x.file.matchAll(/^\.RewardType=(.*)$/gm)].map((m) => m[1]);
      const datas = [...x.file.matchAll(/^\.RewardData=(.*)$/gm)].map((m) => m[1]);
      types.forEach((t, i) => {
        if (t === "TRA_Random_Gold" && !/^(GOLD|CHIPS)\.[1-9][0-9]*$/i.test(datas[i] || ""))
          out.push(`${x.name}: a gold or chips amount needs a number (like GOLD.50000 or CHIPS.20)`);
      });
    }
    const before = previous.find((o) => o.name === c.name && o.role !== "child");
    if (before && before.id !== c.id) out.push(`${c.name} starts over with this change: its players lose their progress in it`);
  }
  for (const o of previous.filter((o) => o.role !== "child"))
    if (!top.some((c) => c.name === o.name)) out.push(`${o.name} is removed: its players lose their progress in it`);
  return out;
}

const timingSafeEqual = async (a, b) => (await sha256(a)) === (await sha256(b));

// Admin requests carry the password ("Authorization: Bearer <password>"). An address that gets it wrong 10 times in an
// hour is turned away for the rest of that hour.
async function adminAllowed(request, env, now) {
  if (!env.ADMIN_KEY) return json({ error: "no admin password is set on this server" }, 404);
  const ip = await connection(request, env);
  const f = await env.DB.prepare("SELECT count, since FROM admin_failures WHERE ip = ?").bind(ip).first();
  const recent = f && now - f.since < 3600;
  if (recent && f.count >= 10) return json({ error: "too many wrong passwords: try again later" }, 429);
  const given = (request.headers.get("Authorization") || "").replace(/^Bearer /, "");
  if (given && (await timingSafeEqual(given, env.ADMIN_KEY))) return null;
  await env.DB.prepare(
    `INSERT INTO admin_failures (ip, count, since) VALUES (?1, 1, ?2)
     ON CONFLICT (ip) DO UPDATE SET count = CASE WHEN ?2 - since < 3600 THEN count + 1 ELSE 1 END,
     since = CASE WHEN ?2 - since < 3600 THEN since ELSE ?2 END`).bind(ip, now).run();
  return json({ error: "wrong password" }, 401);
}

async function handleAdmin(request, env, p, now) {
  if (p.length === 1 || (p.length === 2 && p[1] === "")) return page(ADMIN_PAGE);
  if (p[1] !== "api") return json({}, 404);
  const denied = await adminAllowed(request, env, now);
  if (denied) return denied;
  const current = await eventsText(env);
  const isDefault = current === DEFAULT_EVENTS;
  // GET /admin/api/state: the events file and how every event stands
  if (p[2] === "state" && request.method === "GET") {
    const all = parseEvents(current, now);
    const players = await env.DB.prepare("SELECT COUNT(*) AS n FROM players").first();
    return json({ now, ini: current, isDefault, defaults: DEFAULT_EVENTS, players: players.n, events: await overview(env, all, now, 10) });
  }
  // POST /admin/api/check: what an events file would do, without saving it
  // PUT /admin/api/events: save it (it goes live at once); DELETE: back to the server's own events
  if ((p[2] === "check" && request.method === "POST") || (p[2] === "events" && request.method === "PUT")) {
    const ini = await request.text();
    let all;
    try {
      all = parseEvents(ini, now);
      if (!all.length) throw new Error("there are no events in it");
    } catch (err) {
      return json({ ok: false, error: err.message }, 400);
    }
    const warn = warnings(all, parseEvents(current, now));
    if (p[2] === "events")
      await env.DB.prepare("INSERT INTO config (key, value) VALUES ('events', ?1) ON CONFLICT (key) DO UPDATE SET value = ?1")
        .bind(ini).run();
    const schedule = all.filter((c) => c.role !== "child").map((c) => ({
      name: c.name, kind: c.kind, id: c.id, start: c.start, end: c.end,
      stages: (c.children || []).map((k) => ({ name: k.name, start: k.start, end: k.end, goal: k.goal })),
    }));
    return json({ ok: true, saved: p[2] === "events", warnings: warn, schedule });
  }
  // GET /admin/api/players?q=: players by name or id (the most recently seen first); POST /admin/api/players/name
  // {"id", "name"}: rename one ("" clears the name)
  if (p[2] === "players" && p.length === 3 && request.method === "GET") {
    const q = (new URL(request.url).searchParams.get("q") || "").trim();
    const like = `%${q.replace(/[%_]/g, "")}%`;
    const r = await env.DB.prepare(
      `SELECT p.id, p.name, p.created, p.seen, (SELECT SUM(attempts) FROM progress WHERE player = p.id) AS plays
       FROM players p WHERE ?1 = '' OR p.name LIKE ?2 OR p.id LIKE ?2 ORDER BY p.seen DESC LIMIT 200`).bind(q, like).all();
    return json({ players: r.results });
  }
  if (p[2] === "players" && p[3] === "name" && request.method === "POST") {
    let body;
    try {
      body = await request.json();
    } catch {
      return json({ ok: false, error: "bad request" }, 400);
    }
    if (!(await env.DB.prepare("SELECT id FROM players WHERE id = ?").bind(String(body.id)).first()))
      return json({ ok: false, error: "no such player" }, 404);
    const r = await setName(env, String(body.id), body.name ?? "");
    return json(r, r.ok ? 200 : 400);
  }
  if (p[2] === "events" && request.method === "DELETE") {
    await env.DB.prepare("DELETE FROM config WHERE key = 'events'").run();
    return json({ ok: true });
  }
  return json({}, 404);
}

const page = (html) => new Response(html, {
  headers: {
    "Content-Type": "text/html; charset=utf-8",
    "Content-Security-Policy": "default-src 'self'; style-src 'self' 'unsafe-inline' https://fonts.googleapis.com; " +
      "font-src https://fonts.gstatic.com; script-src 'unsafe-inline'; frame-ancestors 'none'",
    "Cache-Control": "no-store",
  },
});

async function handle(request, env) {
  const url = new URL(request.url);
  if (url.pathname === "/health") return text("ClashMob server\n");
  if (url.pathname === "/favicon.ico") return new Response(null, { status: 204 });
  if (url.pathname === "/theme.css")
    return new Response(THEME_CSS, { headers: { "Content-Type": "text/css; charset=utf-8", "Cache-Control": "public, max-age=300" } });
  // At most 100 requests a minute from one connection (the PER_CONNECTION rate limiter in wrangler.toml: in memory,
  // no database rows). A game starting up sends 20 to 30, then a few a fight.
  if (env.PER_CONNECTION && env.RATE_LIMIT !== "0") {
    const { success } = await env.PER_CONNECTION.limit({ key: await connection(request, env) });
    if (!success) return json({ error: "too many requests: slow down" }, 429);
  }
  // "/sword/api/x" -> ["sword", "api", "x"]; empty parts stay (the game asks for ".../users//saveSlots/0")
  const p = url.pathname.slice(1).split("/").map(decodeURIComponent);
  let now = Math.floor(Date.now() / 1000);
  if (env.TIME_TRAVEL === "1" && request.headers.get("X-ClashMob-Now")) now = parseInt(request.headers.get("X-ClashMob-Now"));
  await ensureSchema(env.DB);
  if (url.pathname === "/") return page(PUBLIC_PAGE);
  // GET /status: the live events for the public page (anyone may read it)
  if (url.pathname === "/status" && request.method === "GET")
    return json({ now, events: await overview(env, parseEvents(await eventsText(env), now), now, 10) });
  if (p[0] === "admin") return handleAdmin(request, env, p, now);
  if (p[0] === "player") return handlePlayer(request, env, p, now);
  // GET /sword/api/timestamp: the server's time (McpServerTimeManagerV3; the game's SecureTime waits for it)
  if (p[0] === "sword" && p[1] === "api" && p[2] === "timestamp" && p.length === 3) return text(iso(now));
  if (p[0] === "sword" && p[1] === "api" && p[2] === "challenges") return handleChallenges(request, env, p, url, now);
  return json({}, 404);
}

export default {
  async fetch(request, env) {
    try {
      return await handle(request, env);
    } catch (err) {
      console.error(err.stack || String(err));
      return json({ error: "server error" }, 500);
    }
  },
};
