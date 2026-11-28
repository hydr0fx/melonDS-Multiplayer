// Lobby server for melonDS Multiplayer (plain Node.js, no dependencies).
// Same API as the Cloudflare worker: /rooms /announce /punch /punches /close
//
// Run:   PORT=8080 node server.js
// Env:   PORT            listen port (default 8080)
//        HOST            bind address (default 0.0.0.0)
//        TRUST_PROXY=1   read client IP from X-Forwarded-For (only behind a reverse proxy)
//        STRICT_OWNER=0  disable owner-IP check for announce/close
//        STRICT_PUNCH_IP=0  allow clients to report a different IP in /punch

"use strict";
const http = require("http");

const PORT = Number(process.env.PORT || 8080);
const HOST = process.env.HOST || "0.0.0.0";
const TRUST_PROXY = process.env.TRUST_PROXY === "1";
const STRICT_OWNER = process.env.STRICT_OWNER !== "0";
const STRICT_PUNCH_IP = process.env.STRICT_PUNCH_IP !== "0";

const ROOM_TTL_MS = 90_000;
const PUNCH_TTL_MS = 45_000;
const MAX_BODY_BYTES = 4096;
const MAX_ROOMS = 1000;
const MAX_PUNCHES_PER_ROOM = 8;
const RATE_LIMIT_PER_MIN = 600;

const CORS_HEADERS = {
  "Access-Control-Allow-Origin": "*",
  "Access-Control-Allow-Methods": "GET, POST, OPTIONS",
  "Access-Control-Allow-Headers": "Content-Type",
  "Content-Type": "application/json",
};

const ROOM_ID_RE = /^[A-Za-z0-9-]{1,32}$/;
const IPV4_RE = /^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})$/;

function parseIPv4(s) {
  const m = IPV4_RE.exec(String(s || ""));
  if (!m) return null;
  const o = m.slice(1).map(Number);
  return o.every((n) => n >= 0 && n <= 255) ? o : null;
}
function isUsableIPv4(s) {
  const o = parseIPv4(s);
  if (!o) return false;
  if (o[0] === 0 || o[0] === 127 || o[0] >= 224) return false;
  if (o[0] === 169 && o[1] === 254) return false;
  return true;
}
function clampInt(v, min, max, fallback) {
  const n = Math.trunc(Number(v));
  if (!Number.isFinite(n)) return fallback;
  return Math.min(max, Math.max(min, n));
}
function cleanStr(v, max, fallback) {
  const s = String(v ?? "").replace(/[\u0000-\u001f\u007f]/g, "").trim().slice(0, max);
  return s || fallback;
}

const rooms = new Map();   // roomId -> entry
const punches = new Map(); // roomId -> [{ip, port, ts}]
const hits = new Map();    // ip -> {windowStart, count}

function prune(now) {
  for (const [id, e] of rooms) if (now - e.updatedAt > ROOM_TTL_MS) rooms.delete(id);
  for (const [roomId, list] of punches) {
    const kept = list.filter((p) => now - p.ts <= PUNCH_TTL_MS);
    if (kept.length > 0 && rooms.has(roomId)) punches.set(roomId, kept);
    else punches.delete(roomId);
  }
  for (const [ip, h] of hits) if (now - h.windowStart > 60_000) hits.delete(ip);
}
function rateLimited(ip, now) {
  const key = ip || "unknown";
  const h = hits.get(key);
  if (!h || now - h.windowStart > 60_000) {
    hits.set(key, { windowStart: now, count: 1 });
    return false;
  }
  h.count += 1;
  return h.count > RATE_LIMIT_PER_MIN;
}
function isOwner(entry, ip) {
  if (!STRICT_OWNER) return true;
  return !entry || !entry.ownerIp || entry.ownerIp === ip;
}

function clientIp(req) {
  let ip = "";
  if (TRUST_PROXY) {
    const xff = String(req.headers["x-forwarded-for"] || "").split(",")[0].trim();
    if (xff) ip = xff;
  }
  if (!ip) ip = req.socket.remoteAddress || "";
  if (ip.startsWith("::ffff:")) ip = ip.slice(7);
  return ip;
}

function send(res, obj, status = 200) {
  res.writeHead(status, CORS_HEADERS);
  res.end(JSON.stringify(obj));
}

function readJson(req) {
  return new Promise((resolve) => {
    let size = 0;
    const chunks = [];
    let done = false;
    const finish = (r) => { if (!done) { done = true; resolve(r); } };
    req.on("data", (c) => {
      size += c.length;
      if (size > MAX_BODY_BYTES) {
        finish({ error: "payload too large", status: 413 });
        chunks.length = 0; // drop the rest, keep reading so the 413 response can be sent
        return;
      }
      if (!done) chunks.push(c);
    });
    req.on("end", () => {
      try {
        const body = JSON.parse(Buffer.concat(chunks).toString("utf8"));
        if (!body || typeof body !== "object" || Array.isArray(body)) throw new Error("not an object");
        finish({ body });
      } catch {
        finish({ error: "invalid json", status: 400 });
      }
    });
    req.on("error", () => finish({ error: "invalid json", status: 400 }));
  });
}

async function handle(req, res) {
  if (req.method === "OPTIONS") {
    res.writeHead(204, CORS_HEADERS);
    return res.end();
  }
  const now = Date.now();
  const ip = clientIp(req);
  const url = new URL(req.url, "http://localhost");
  prune(now);
  if (rateLimited(ip, now)) return send(res, { error: "rate limited" }, 429);

  if (url.pathname === "/rooms" && req.method === "GET") {
    const list = Array.from(rooms.values()).map(({ ownerIp, ...pub }) => pub);
    return send(res, { rooms: list });
  }

  if (url.pathname === "/announce" && req.method === "POST") {
    const r = await readJson(req);
    if (r.error) return send(res, { error: r.error }, r.status);
    const body = r.body;
    const roomId = String(body.roomId ?? "");
    if (!ROOM_ID_RE.test(roomId)) return send(res, { error: "bad roomId" }, 400);
    const existing = rooms.get(roomId);
    if (existing && !isOwner(existing, ip)) return send(res, { error: "room taken" }, 409);
    if (!existing && rooms.size >= MAX_ROOMS) return send(res, { error: "too many rooms" }, 503);
    const addr = isUsableIPv4(body.addr) ? String(body.addr) : isUsableIPv4(ip) ? ip : "";
    if (!addr) return send(res, { error: "no usable address" }, 400);
    const maxPlayers = clampInt(body.maxPlayers, 2, 4, 4);
    rooms.set(roomId, {
      roomId,
      roomName: cleanStr(body.roomName, 48, "Lobby"),
      hostName: cleanStr(body.hostName, 16, "Host"),
      game: cleanStr(body.game, 64, "Nintendo DS"),
      romHash: cleanStr(body.romHash, 32, ""),
      addr,
      port: clampInt(body.port, 1, 65535, 7090),
      players: clampInt(body.players, 0, maxPlayers, 1),
      maxPlayers,
      hasPassword: Boolean(body.hasPassword),
      updatedAt: now,
      ownerIp: existing?.ownerIp || ip,
    });
    return send(res, { ok: true });
  }

  if (url.pathname === "/punch" && req.method === "POST") {
    const r = await readJson(req);
    if (r.error) return send(res, { error: r.error }, r.status);
    const body = r.body;
    const roomId = String(body.roomId ?? "");
    if (!ROOM_ID_RE.test(roomId)) return send(res, { error: "bad roomId" }, 400);
    if (!rooms.has(roomId)) return send(res, { ok: true });
    let clientAddr = String(body.ip ?? "");
    if (STRICT_PUNCH_IP && parseIPv4(ip)) clientAddr = ip;
    const clientPort = clampInt(body.port, 0, 65535, 0);
    if (isUsableIPv4(clientAddr) && clientPort >= 1) {
      const list = punches.get(roomId) || [];
      if (!list.some((p) => p.ip === clientAddr && p.port === clientPort) && list.length < MAX_PUNCHES_PER_ROOM) {
        list.push({ ip: clientAddr, port: clientPort, ts: now });
        punches.set(roomId, list);
      }
    }
    return send(res, { ok: true });
  }

  if (url.pathname === "/punches" && req.method === "GET") {
    const roomId = String(url.searchParams.get("roomId") || "");
    if (!ROOM_ID_RE.test(roomId)) return send(res, { peers: [] });
    const room = rooms.get(roomId);
    if (room && !isOwner(room, ip)) return send(res, { peers: [] });
    const list = punches.get(roomId) || [];
    punches.delete(roomId);
    return send(res, { peers: list.map(({ ip: pip, port }) => ({ ip: pip, port })) });
  }

  if (url.pathname === "/close" && req.method === "POST") {
    const r = await readJson(req);
    if (r.error) return send(res, { error: r.error }, r.status);
    const rid = String(r.body.roomId ?? "");
    if (ROOM_ID_RE.test(rid)) {
      const room = rooms.get(rid);
      if (isOwner(room, ip)) {
        rooms.delete(rid);
        punches.delete(rid);
      }
    }
    return send(res, { ok: true });
  }

  if (url.pathname === "/health") return send(res, { ok: true, rooms: rooms.size });

  return send(res, { error: "not found" }, 404);
}

const server = http.createServer((req, res) => {
  handle(req, res).catch(() => {
    try { send(res, { error: "internal error" }, 500); } catch { /* ignore */ }
  });
});
server.headersTimeout = 10_000;
server.requestTimeout = 10_000;
server.listen(PORT, HOST, () => console.log(`melonDS lobby listening on ${HOST}:${PORT}`));
