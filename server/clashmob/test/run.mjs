// Plays the game's side of ClashMobs against a running server, as several players, and checks the answers.
//   npm run dev      (in another terminal; .dev.vars with ADMIN_KEY=... and TIME_TRAVEL=1 also tests stages,
//                     tournaments and trials over time)
//   npm test         (BASE=<url> and ADMIN_KEY=<key> to test another server)
import { readFileSync } from "node:fs";
import { randomBytes } from "node:crypto";

const BASE = process.env.BASE || "http://127.0.0.1:8787";
let vars = "";
try {
  vars = readFileSync(new URL("../.dev.vars", import.meta.url), "utf8");
} catch {}
const ADMIN_KEY = process.env.ADMIN_KEY ?? vars.match(/^ADMIN_KEY\s*=\s*"?([^"\r\n]+)/m)?.[1];
const TIME_TRAVEL = /^TIME_TRAVEL\s*=\s*"?1/m.test(vars);

let failed = 0;
function check(what, ok, detail = "") {
  console.log(`${ok ? "ok  " : "FAIL"} ${what}${detail ? `  (${detail})` : ""}`);
  if (!ok) failed++;
}

const newPlayer = (name) => ({ name, id: `test-${name}-${randomBytes(4).toString("hex")}`, key: randomBytes(16).toString("hex") });
let clock = 0;  // the server's time for these requests (0: its own)

async function call(method, path, player, body) {
  const headers = { "Content-Type": "application/json" };
  if (player) Object.assign(headers, { "X-ClashMob-Player": player.id, "X-ClashMob-Key": player.key });
  if (clock) headers["X-ClashMob-Now"] = String(clock);
  const r = await fetch(BASE + path, { method, headers, body });
  const t = await r.text();
  let j = null;
  try { j = JSON.parse(t); } catch {}
  return { status: r.status, text: t, json: j };
}
const admin = (method, path, body) =>
  fetch(BASE + path, { method, headers: { Authorization: `Bearer ${ADMIN_KEY}`, ...(clock ? { "X-ClashMob-Now": String(clock) } : {}) }, body })
    .then(async (r) => ({ status: r.status, text: await r.text() }));
const C = "/sword/api/challenges";
const list = async (p) => (await call("GET", C, p)).json;
const join = (id, p) => call("POST", `${C}/${id}/users//saveSlots/0`, p);
const play = async (id, p, score, done = true) => {
  clock && (clock += 20);  // plays are at least MIN_PLAY_SECONDS apart
  return call("POST", `${C}/${id}/users//saveSlots/0/updateProgress?didComplete=${done ? "True" : "False"}&goalProgress=${score}`, p);
};
const statuses = async (id, p) => (await call("POST", `${C}/${id}/users?wantsParentChildInfo=true`, p, "[ ]")).json;
const find = (all, name, suffix = "") => all.find((e) => new RegExp(`^cm-${name}-\\d+${suffix}$`).test(e.challengeId));

// ---- the default events ----
check("server answers", (await call("GET", "/")).text.includes("ClashMob"), BASE);
const A = newPlayer("a"), B = newPlayer("b");
const all = await list(A);
check("event list", Array.isArray(all), `${all?.length} challenges`);
const dk = find(all, "darkknight"), dk1 = find(all, "darkknight", "-s1"), dk2 = find(all, "darkknight", "-s2");
check("Dark Knight ClashMob: a parent with 3 stages", dk && dk.childChallengeList.length === 3 && dk.files.length === 0 && dk.challengeType === "SOCIAL");
check("its stages name the parent", dk1?.parentChallengeId === dk.challengeId && dk2?.parentChallengeId === dk.challengeId);
check("stage 1 is being played", dk.activeChildChallengeId === dk1.challengeId);
check("co-op gate", dk1.childChallengeGatingType === "SUCCESS" && dk1.challengeRatingType === "TOTAL_PROGRESS");
const f2 = (await call("GET", `${C}/${dk2.challengeId}/file/x.ib3`, A)).text;
check("stage 2's file: its own boss level and only its reward", f2.includes("BossLevel=15") && !f2.includes("BossLevel=10") &&
  f2.includes("TRA_GrabBag_LargeGem") && !f2.includes("TRA_Gold_Large") && f2.includes("BossObj=10ft_SnS_BlackKnight"));
const gol = find(all, "goliath");
check("Goliath: a Trial (solo, best play)", gol?.challengeType === "SOLO" && gol.challengeRatingType === "HIGH_PROGRESS" && gol.childChallengeList.length === 0);
const aeg = find(all, "aegis"), aeg1 = find(all, "aegis", "-s1"), aeg5 = find(all, "aegis", "-s5");
check("Aegis Tournament: 5 stages, top 50% go on", aeg?.childChallengeList.length === 5 && aeg1.childChallengeGatingType === "TOP_N_PERCENT" &&
  aeg1.childChallengeGatingValue === 50);
check("the final's reward is Anarchax", (await call("GET", `${C}/${aeg5.challengeId}/file/x.ib3`, A)).text.includes(".RewardData=Sword_222"));
const kids = (await call("GET", `${C}/${dk.challengeId}/children`, A)).json;
check(".../children: the parent and its stages", kids?.length === 4 && kids[0].challengeId === dk.challengeId);

check("wrong key refused", (await call("GET", C, { id: A.id, key: "0".repeat(32) })).status === 403);
check("bad id refused", (await call("GET", C, { id: "x", key: A.key })).status === 400);
check("no player, no joining", (await call("POST", `${C}/${dk.challengeId}/users//saveSlots/0`)).status === 401);
check("unknown event", (await call("GET", `${C}/cm-nothing-1`, A)).status === 404);

// ---- over time (an uploaded set of small events, with the server's clock moved) ----
if (ADMIN_KEY && TIME_TRAVEL) {
  const H = 7200;  // the uploaded events last 2 hours
  const base = (Math.floor(Date.now() / 1000 / H) + 1000 + Math.floor(Math.random() * 100000)) * H;
  clock = base + 60;
  const ini = `
[mob]
Type=ClashMob
Hours=2
Title=Test mob
BattleType=BT_KillNBosses
[mob.1]
Goal=2
.RewardType=TRA_Gold_Large
.RewardData=
.RewardGoal=1
[mob.2]
Goal=2
.RewardType=TRA_GrabBag_Uber
.RewardData=
.RewardGoal=1
[cup]
Type=Tournament
Hours=2
TopPercent=50
Title=Test cup
BattleType=BT_Kill1Boss
BossHealth=1000
[cup.1]
.RewardType=TRA_Chips_Small
.RewardData=
.RewardGoal=1
[cup.2]
.RewardType=TRA_Item_Fixed
.RewardData=Sword_222
.RewardGoal=1
[trial]
Type=Trial
Hours=2
Title=Test trial
BattleType=BT_Kill1Boss
BossHealth=1000
MaxPlays=2
.RewardType=TRA_Gold_Small
.RewardData=
.RewardGoal=100
`;
  const put = await admin("PUT", "/admin/events", ini);
  check("admin: upload events", put.status === 200, put.text.trim());
  check("admin: a Trial with stages refused", (await admin("PUT", "/admin/events", "[t]\nType=Trial\n[t.1]\nGoal=1\n")).status === 400);

  // ClashMob: the mob clears stage 1, stage 2 opens; everyone (late joiners too) is in the stage being played
  const P = [newPlayer("a"), newPlayer("b"), newPlayer("c")];
  let l = await list(P[0]);
  const mob = find(l, "mob"), m1 = find(l, "mob", "-s1"), m2 = find(l, "mob", "-s2");
  await join(mob.challengeId, P[0]);
  let st = await statuses(mob.challengeId, P[0]);
  check("joining a ClashMob puts the player in stage 1", st.some((s) => s.challengeId === m1.challengeId) && !st.some((s) => s.challengeId === m2.challengeId));
  await play(m1.challengeId, P[0], 1);
  check("a play in stage 2 before it opens does not count", (await play(m2.challengeId, P[0], 1)).json.goalProgress === 0);
  await join(mob.challengeId, P[1]);
  await play(m1.challengeId, P[1], 1);
  l = await list(P[0]);
  const m1b = find(l, "mob", "-s1"), mobb = find(l, "mob");
  check("stage 1 cleared: completed and successful", m1b.completed && m1b.successful && m1b.completedDate !== "", `${m1b.goalCurrentValue}/${m1b.goalValue}`);
  check("mob size = players in the stage", m1b.attempts === 2, `${m1b.attempts}`);
  check("stage 2 is now being played", mobb.activeChildChallengeId === m2.challengeId && !mobb.completed);
  st = await statuses(mob.challengeId, P[0]);
  check("players are moved into stage 2", st.some((s) => s.challengeId === m2.challengeId));
  await join(mob.challengeId, P[2]);
  st = await statuses(mob.challengeId, P[2]);
  check("a late joiner goes straight into stage 2", st.some((s) => s.challengeId === m2.challengeId) && !st.some((s) => s.challengeId === m1.challengeId));
  clock += 700;
  check("stage 1 no longer scores once cleared", (await play(m1.challengeId, P[1], 1)).json.goalProgress === 1);
  await play(m2.challengeId, P[2], 1);
  await play(m2.challengeId, P[0], 1);
  l = await list(P[0]);
  check("both stages cleared: the ClashMob is won", find(l, "mob").completed && find(l, "mob").successful && find(l, "mob", "-s2").successful);

  // Tournament: stage 1 ranks best scores; the top half go on to stage 2
  const T = [newPlayer("t1"), newPlayer("t2"), newPlayer("t3"), newPlayer("t4")];
  l = await list(T[0]);
  const cup = find(l, "cup"), c1 = find(l, "cup", "-s1"), c2 = find(l, "cup", "-s2");
  check("tournament stage 1 runs the first hour", Date.parse(c1.endDate) / 1000 === base + 3600 && Date.parse(c2.startDate) / 1000 === base + 3600);
  for (const [i, p] of T.entries()) {
    await join(cup.challengeId, p);
    await play(c1.challengeId, p, (i + 1) * 100);
  }
  await play(c1.challengeId, T[0], 50);  // a worse play does not lower the best
  const s4 = (await statuses(cup.challengeId, T[3])).find((s) => s.challengeId === c1.challengeId);
  const s1 = (await statuses(cup.challengeId, T[0])).find((s) => s.challengeId === c1.challengeId);
  check("best play counts", s1.highGoalProgress === 100 && s4.highGoalProgress === 400);
  check("ranks: the best is first, in the top half", s4.rank === 1 && s4.percentRank >= 50, `rank ${s4.rank}, ${s4.percentRank}%`);
  check("the worst is below the gate", s1.rank === 4 && s1.percentRank < 50, `rank ${s1.rank}, ${s1.percentRank}%`);
  check("damage capped at BossHealth", (await play(c1.challengeId, T[1], 99999)).json.highGoalProgress === 1000);
  clock = base + 3600 + 60;  // stage 2
  l = await list(T[2]);
  check("stage 1 over: completed, rewards for those who went through", find(l, "cup", "-s1").completed && find(l, "cup", "-s1").successful);
  check("stage 2 is being played", find(l, "cup").activeChildChallengeId === c2.challengeId);
  const in2 = async (p) => (await statuses(cup.challengeId, p)).some((s) => s.challengeId === c2.challengeId);
  // after the cap above, t2 has 1000: t2 and t4 are the top half
  check("the top half are in stage 2", (await in2(T[1])) && (await in2(T[3])), "t2, t4");
  check("the others are not", !(await in2(T[0])) && !(await in2(T[2])), "t1, t3");
  check("joining stage 2 without qualifying is refused", (await join(c2.challengeId, T[0])).status === 403);
  clock = base + 3600 + 700;  // past stage 1's end and its grace
  check("a stage 1 play after it ended does not count", (await play(c1.challengeId, T[2], 5000)).json.highGoalProgress === 300);

  // Trial: best play, MaxPlays
  const tr = find(l, "trial"), X = newPlayer("x");
  await join(tr.challengeId, X);
  await play(tr.challengeId, X, 150);
  await play(tr.challengeId, X, 90);
  const t3 = (await play(tr.challengeId, X, 900)).json;
  check("trial: best play, and no score after MaxPlays", t3.highGoalProgress === 150 && t3.numAttempts === 3, `best ${t3.highGoalProgress}`);
  const r = (await call("POST", `${C}/${tr.challengeId}/users//saveSlots/0/updateReward?rewardValue=1`, X)).json;
  check("reward recorded", r.userAwardGiven === 1);

  const stats = await admin("GET", "/admin/stats");
  check("admin: stats", stats.status === 200 && JSON.parse(stats.text).challenges.length === 7);
  check("admin: reset events", (await admin("DELETE", "/admin/events")).status === 200);
  clock = 0;
  check("admin: wrong key hidden", (await fetch(BASE + "/admin/stats", { headers: { Authorization: "Bearer nope" } })).status === 404);
} else {
  console.log("(no ADMIN_KEY / TIME_TRAVEL=1 in .dev.vars: the tests over time are skipped)");
}

console.log(failed ? `\n${failed} FAILED` : "\nall passed");
process.exit(failed ? 1 : 0);
