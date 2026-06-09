#!/usr/bin/env node
/**
 * leo-channel — bidirectional Claude Code channel for the ESP32 voice buddy → Leo.
 *
 * The max-side buddy server transcribes mic audio (Whisper) and, when the buddy
 * is in "Leo" mode, POSTs the text here. We surface it as a notifications/claude/
 * channel event so it becomes a turn in this (Leo) session. Leo replies via the
 * leo_chat_reply tool, which POSTs back to the buddy server's /chat_reply so the
 * reply shows on the e-paper. Node port of max's bun-based ss-chat-channel.
 *
 * Listens on 0.0.0.0:8804 (gated by X-Webhook-Token). Reachable from max on the LAN.
 */
import { Server } from '@modelcontextprotocol/sdk/server/index.js';
import { StdioServerTransport } from '@modelcontextprotocol/sdk/server/stdio.js';
import { CallToolRequestSchema, ListToolsRequestSchema } from '@modelcontextprotocol/sdk/types.js';
import { randomBytes } from 'node:crypto';
import { appendFileSync, existsSync, mkdirSync, readFileSync } from 'node:fs';
import { homedir } from 'node:os';
import { join } from 'node:path';
import http from 'node:http';

const STATE_DIR = process.env.LEO_CHANNEL_STATE_DIR
  ?? join(homedir(), '.claude', 'channels', 'leo-channel');
const ENV_FILE = join(STATE_DIR, '.env');
mkdirSync(STATE_DIR, { recursive: true });

if (existsSync(ENV_FILE)) {
  for (const line of readFileSync(ENV_FILE, 'utf8').split('\n')) {
    const m = line.match(/^\s*([A-Z_][A-Z0-9_]*)\s*=\s*(.*)\s*$/);
    if (m && !process.env[m[1]]) process.env[m[1]] = m[2].replace(/^["']|["']$/g, '');
  }
}

let token = process.env.LEO_CHANNEL_TOKEN?.trim();
if (!token) {
  token = randomBytes(16).toString('hex');
  appendFileSync(ENV_FILE, `\nLEO_CHANNEL_TOKEN=${token}\n`);
  process.stderr.write(`[leo-channel] generated LEO_CHANNEL_TOKEN=${token} (saved to ${ENV_FILE})\n`);
}
const PORT = parseInt(process.env.LEO_CHANNEL_PORT || '8804', 10);
const DEFAULT_REPLY = (process.env.LEO_REPLY_URL || '').trim();

// Per-request reply URLs (the buddy server tells us where to send Leo's reply).
const PENDING = new Map();
const TTL_MS = 10 * 60 * 1000;
setInterval(() => {
  const now = Date.now();
  for (const [k, v] of PENDING) if (v.exp < now) PENDING.delete(k);
}, 60_000).unref?.();

const mcp = new Server(
  { name: 'leo-channel', version: '0.1.0' },
  {
    capabilities: {
      tools: {},
      experimental: { 'claude/channel': {}, 'claude/channel/permission': {} },
    },
    instructions: [
      'Events from the leo-channel arrive as <channel source="leo" request_id="...">.',
      'This is August speaking out loud to you (Leo) through his ESP32 desk buddy —',
      'treat it as a direct message from August and respond as you normally would.',
      'Reply by calling the leo_chat_reply tool with that exact request_id and your reply text.',
      'Keep replies conversational and short (they are read aloud / shown on a tiny screen).',
      'Call leo_chat_reply exactly once per request_id.',
    ].join(' '),
  },
);

mcp.setRequestHandler(ListToolsRequestSchema, async () => ({
  tools: [
    {
      name: 'leo_chat_reply',
      description:
        'Reply to a leo-channel voice message. Call exactly once per <channel source="leo"> ' +
        'event, using the request_id from the channel attributes.',
      inputSchema: {
        type: 'object',
        required: ['request_id', 'text'],
        properties: {
          request_id: { type: 'string', description: 'request_id from the channel event.' },
          text: { type: 'string', description: 'Reply text (shown on the buddy e-paper).' },
        },
      },
    },
  ],
}));

mcp.setRequestHandler(CallToolRequestSchema, async (req) => {
  if (req.params.name !== 'leo_chat_reply') {
    return { content: [{ type: 'text', text: `unknown tool: ${req.params.name}` }], isError: true };
  }
  const { request_id, text } = req.params.arguments ?? {};
  if (!request_id || typeof text !== 'string') {
    return { content: [{ type: 'text', text: 'request_id and text required' }], isError: true };
  }
  const pending = PENDING.get(request_id);
  PENDING.delete(request_id);
  const url = pending?.url || DEFAULT_REPLY;
  if (!url) return { content: [{ type: 'text', text: 'no reply_url configured' }], isError: true };
  try {
    const res = await fetch(url, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json', 'X-Webhook-Token': token },
      body: JSON.stringify({ request_id, text }),
    });
    if (!res.ok) {
      return { content: [{ type: 'text', text: `reply POST failed: HTTP ${res.status}` }], isError: true };
    }
    return { content: [{ type: 'text', text: `delivered (request_id=${request_id})` }] };
  } catch (err) {
    return { content: [{ type: 'text', text: `reply POST error: ${err.message}` }], isError: true };
  }
});

await mcp.connect(new StdioServerTransport());

function clip(s, max) {
  const t = String(s ?? '');
  return t.length > max ? t.slice(0, max - 1) + '…' : t;
}

http.createServer((req, res) => {
  if (req.method !== 'POST') { res.writeHead(405); res.end('method not allowed'); return; }
  const presented = (req.headers['x-webhook-token'] || '').toString().trim();
  if (!presented || presented !== token) { res.writeHead(403); res.end('forbidden'); return; }
  let body = '';
  req.on('data', (c) => { body += c; });
  req.on('end', async () => {
    let j;
    try { j = JSON.parse(body); } catch { res.writeHead(400); res.end('bad json'); return; }
    const requestId = j.request_id ?? j.requestId;
    const text = clip(j.text ?? j.transcript, 4000).trim();
    if (!requestId || !text) { res.writeHead(400); res.end('missing request_id or text'); return; }
    if (j.reply_url) PENDING.set(String(requestId), { url: String(j.reply_url), exp: Date.now() + TTL_MS });
    try {
      await mcp.notification({
        method: 'notifications/claude/channel',
        params: { content: text, meta: { request_id: String(requestId), source: 'leo', host: 'leo' } },
      });
    } catch (err) {
      process.stderr.write(`[leo-channel] notify error: ${err.message}\n`);
    }
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ ok: true, request_id: String(requestId) }));
  });
}).listen(PORT, '0.0.0.0', () => process.stderr.write(`[leo-channel] listening on http://0.0.0.0:${PORT}\n`));
