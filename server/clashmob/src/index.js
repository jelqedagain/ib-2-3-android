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
// Admin (when the ADMIN_KEY secret is set; "Authorization: Bearer <key>"): GET/PUT/DELETE /admin/events (the events
// ini), GET /admin/stats. For tests only, TIME_TRAVEL=1 lets requests set the server's clock (X-ClashMob-Now).
import DEFAULT_EVENTS from "./events.js";

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
];

let schemaReady = null;
function ensureSchema(db) {
  schemaReady ??= db.batch(SCHEMA.map((s) => db.prepare(s))).catch((e) => {
    schemaReady = null;
    throw e;
  });
  return schemaReady;
}

// ---- the events ----

const SERVER_KEYS = ["Type", "Mode", "Days", "Hours", "Start", "StageHours", "Goal", "Score", "MaxScore", "TopPercent"];
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
    const start = now < anchor ? anchor : anchor + Math.floor((now - anchor) / period) * period;
    // A tournament's stages last StageHours each (else they share the period), and it is over after the last one
    const stageLen = kind === "Tournament" && stages.length
      ? Math.min(Math.round((parseFloat(k.StageHours) || hours / stages.length) * 3600), Math.floor(period / stages.length)) : 0;
    const end = stageLen ? start + stageLen * stages.length : start + period;
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
  return out;
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
    env.DB.prepare(`SELECT challenge, COUNT(*) AS players, SUM(progress) AS total, SUM(successful > 0) AS scorers
                    FROM progress WHERE challenge IN (${marks}) GROUP BY challenge`).bind(...ids),
    env.DB.prepare(`SELECT challenge, at FROM won WHERE challenge IN (${marks})`).bind(...ids),
  ]);
  for (const r of sums.results) Object.assign(out[r.challenge], { total: r.total || 0, players: r.players, scorers: r.scorers || 0 });
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
      filename: "BattleEvent_1.4.ib3",
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

const join = (env, c, player, slot, now) =>
  env.DB.prepare(
    `INSERT INTO progress (challenge, player, slot, accept_time, update_time) VALUES (?1, ?2, ?3, ?4, ?4)
     ON CONFLICT (challenge, player) DO UPDATE SET slot = excluded.slot, update_time = excluded.update_time`)
    .bind(c.id, player, slot, now).run();

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

// The player making the request: their id, null if the request has none, or an error Response.
async function authenticate(request, env, now) {
  const id = request.headers.get("X-ClashMob-Player"), key = request.headers.get("X-ClashMob-Key");
  if (!id && !key) return null;
  if (!/^[A-Za-z0-9_-]{4,64}$/.test(id || "") || !/^[0-9a-f]{32,128}$/.test(key || ""))
    return json({ error: "bad player id or key" }, 400);
  const h = await sha256(key);
  const row = await env.DB.prepare("SELECT key_hash FROM players WHERE id = ?").bind(id).first();
  if (!row) {
    await env.DB.prepare("INSERT OR IGNORE INTO players (id, key_hash, created, seen) VALUES (?, ?, ?, ?)")
      .bind(id, h, now, now).run();
    const again = await env.DB.prepare("SELECT key_hash FROM players WHERE id = ?").bind(id).first();
    if (!again || again.key_hash !== h) return json({ error: "wrong key for this player" }, 403);
  } else if (row.key_hash !== h) {
    return json({ error: "wrong key for this player" }, 403);
  } else {
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
  if (add > 0 && c.kind === "ClashMob") {
    const after = (await community(env, [c]))[c.id];
    if (after.total >= c.goal) await env.DB.prepare("INSERT OR IGNORE INTO won (challenge, at) VALUES (?, ?)").bind(c.id, now).run();
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

async function handleAdmin(request, env, p, now) {
  if (!env.ADMIN_KEY || request.headers.get("Authorization") !== `Bearer ${env.ADMIN_KEY}`) return json({}, 404);
  if (p[1] === "events") {
    if (request.method === "GET") return text(await eventsText(env));
    if (request.method === "DELETE") {
      await env.DB.prepare("DELETE FROM config WHERE key = 'events'").run();
      return text("events reset to the defaults\n");
    }
    if (request.method === "PUT" || request.method === "POST") {
      const ini = await request.text();
      let all;
      try {
        all = parseEvents(ini, now);
      } catch (err) {
        return text(`${err.message}\n`, 400);
      }
      if (!all.length) return text("no events in that file\n", 400);
      await env.DB.prepare("INSERT INTO config (key, value) VALUES ('events', ?1) ON CONFLICT (key) DO UPDATE SET value = ?1")
        .bind(ini).run();
      const top = all.filter((c) => c.role !== "child");
      return text(`saved ${top.length} event(s): ${top.map((c) => `${c.id} (${c.kind}${c.children ? `, ${c.children.length} stages` : ""})`).join(", ")}\n`);
    }
  }
  if (p[1] === "stats" && request.method === "GET") {
    const all = parseEvents(await eventsText(env), now);
    const com = await community(env, all);
    const out = [];
    for (const c of all) {
      const top = await env.DB.prepare(
        `SELECT player, ${scoreCol(c)} AS score, attempts FROM progress WHERE challenge = ? ORDER BY ${scoreCol(c)} DESC LIMIT 10`).bind(c.id).all();
      const st = standing(c, com, now);
      out.push({ id: c.id, kind: c.kind, role: c.role, start: iso(c.start), end: iso(c.end), goal: c.goal, ...com[c.id],
        won: st.won, completed: st.completed, ...(c.role === "parent" ? { activeStage: st.active + 1 } : {}), top: top.results });
    }
    const players = await env.DB.prepare("SELECT COUNT(*) AS n FROM players").first();
    return json({ players: players.n, challenges: out });
  }
  return json({}, 404);
}

async function handle(request, env) {
  const url = new URL(request.url);
  if (url.pathname === "/" || url.pathname === "/health") return text("ClashMob server\n");
  // "/sword/api/x" -> ["sword", "api", "x"]; empty parts stay (the game asks for ".../users//saveSlots/0")
  const p = url.pathname.slice(1).split("/").map(decodeURIComponent);
  let now = Math.floor(Date.now() / 1000);
  if (env.TIME_TRAVEL === "1" && request.headers.get("X-ClashMob-Now")) now = parseInt(request.headers.get("X-ClashMob-Now"));
  await ensureSchema(env.DB);
  if (p[0] === "admin") return handleAdmin(request, env, p, now);
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
