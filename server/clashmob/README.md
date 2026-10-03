# ClashMob server

The community ClashMob server for the IB3 Android port: a Cloudflare Worker with a D1 database. The app sends the
game's ClashMob requests here (`src/game/clashmob.cpp`); without a connection it falls back to offline ClashMobs.

It runs ClashMob 2.0's three kinds of event:

- **Trials**: solo. Each play is scored, and the best one earns bronze, silver and gold rewards.
- **ClashMobs**: co-op, in stages. The whole mob works toward one goal; reaching it opens the next stage. Each
  stage's reward goes to everyone who played it once the mob clears it.
- **Aegis Tournaments**: timed stages. Each stage ranks the players' best scores, and only the top share go on to
  the next stage.

## Pages

- `/`: the live events, for anyone.
- `/admin`: the admin page. Shows each event live, edits the events (forms, or the events file as text), checks
  a change before it goes live, and publishes it. It needs the admin password (the `ADMIN_KEY` secret); an
  address that gets the password wrong 10 times in an hour is turned away for the rest of the hour.

Published events go live at once; games pick them up when they start and every 20 minutes. Changing an
event's schedule, or removing it, starts it over: its players lose their progress in it (the admin page warns
first). `src/events.js` has the events file's format and the server's default events.

## Running it

```
npm install
npm run dev        # this PC, with a local database (http://127.0.0.1:8787)
npm test           # plays it as several players; with .dev.vars holding ADMIN_KEY=... and TIME_TRAVEL=1 it
                   # also moves the clock to test stages, tournaments and trials over time (only locally)
```

Point a phone at the local server for testing: `adb reverse tcp:8787 tcp:8787` and
`adb shell setprop debug.ibport.clashmobserver http://127.0.0.1:8787` (`''` to go back to the app's own server).

## Putting it online

```
npx wrangler login
npx wrangler d1 create clashmob          # once: put the database_id it prints in wrangler.toml
npx wrangler deploy
npx wrangler secret put ADMIN_KEY        # the admin password (asked for, not echoed)
```

Abuse limits: 100 requests a minute and 10 new players a day per connection, 10 wrong admin passwords an hour per
connection, one scoring fight every 20 seconds per player, and caps on what one fight can score. Connections are
counted by a fingerprint of their address (an HMAC with the IP_SALT secret: `npx wrangler secret put IP_SALT`), never
the address itself.

Free plan limits: 100,000 requests and 5 million database rows read a day. Each challenge's totals are kept
as players play, so a request reads a handful of rows; a play session is about 30 to 50 requests.
