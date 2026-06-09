#!/usr/bin/env bun
/**
 * voice-channel — one-way Claude Code channel for spoken input from the ESP32 buddy.
 *
 * The max-side buddy server transcribes mic audio (Whisper) and POSTs the text here;
 * we forward it as a notifications/claude/channel event over stdio so it surfaces as a
 * turn in the always-on ss-channels session ("Max"). Speaking to the buddy = a message
 * to Max.
 *
 * Mirrors the proven vikunja-channel: listens on 0.0.0.0:8803, gated by X-Webhook-Token
 * (the actual security boundary), one-way (no reply tool).
 */
import { Server } from '@modelcontextprotocol/sdk/server/index.js';
import { StdioServerTransport } from '@modelcontextprotocol/sdk/server/stdio.js';
import { randomBytes } from 'node:crypto';
import { appendFileSync, existsSync, mkdirSync, readFileSync } from 'node:fs';
import { homedir } from 'node:os';
import { join } from 'node:path';

const STATE_DIR = process.env.VOICE_CHANNEL_STATE_DIR
  ?? join(homedir(), '.claude', 'channels', 'voice-channel');
const ENV_FILE = join(STATE_DIR, '.env');
mkdirSync(STATE_DIR, { recursive: true, mode: 0o700 });

if (!process.env.VOICE_WEBHOOK_TOKEN && existsSync(ENV_FILE)) {
  for (const line of readFileSync(ENV_FILE, 'utf8').split('\n')) {
    const m = line.match(/^\s*([A-Z_][A-Z0-9_]*)\s*=\s*(.*)\s*$/);
    if (m && !process.env[m[1]]) process.env[m[1]] = m[2].replace(/^["']|["']$/g, '');
  }
}

let token = process.env.VOICE_WEBHOOK_TOKEN?.trim();
if (!token) {
  token = randomBytes(16).toString('hex');
  appendFileSync(ENV_FILE, `\nVOICE_WEBHOOK_TOKEN=${token}\n`, { mode: 0o600 });
  // stderr only — stdout is the MCP JSON-RPC transport.
  process.stderr.write(
    `[voice-channel] Generated VOICE_WEBHOOK_TOKEN=${token} (saved to ${ENV_FILE})\n`,
  );
}
process.env.VOICE_WEBHOOK_TOKEN = token;

const mcp = new Server(
  { name: 'voice-channel', version: '0.1.0' },
  {
    capabilities: {
      experimental: {
        'claude/channel': {},
        'claude/channel/permission': {},
      },
    },
    instructions: [
      'Events from the voice-channel arrive as <channel source="voice" text="...">.',
      'This is August speaking out loud to you through the ESP32 desk buddy — treat it as a',
      'direct message from August in this session and respond as you normally would in the',
      'ss-channels session (act on it, answer it, or acknowledge it).',
      'There is NO reply tool on this channel — do not try to respond through it; reply in your',
      'normal session output / the other channels loaded here.',
    ].join(' '),
  },
);

await mcp.connect(new StdioServerTransport());

function clip(s: unknown, max: number): string {
  const str = String(s ?? '');
  return str.length > max ? str.slice(0, max - 1) + '…' : str;
}

Bun.serve({
  port: 8803,
  hostname: '0.0.0.0', // reachable from the buddy server (container/LAN); token gate is the boundary
  async fetch(req) {
    if (req.method !== 'POST') {
      return new Response('method not allowed', { status: 405 });
    }
    const presented = req.headers.get('x-webhook-token')?.trim();
    if (!presented || presented !== token) {
      return new Response('forbidden', { status: 403 });
    }

    let body: any;
    try {
      body = await req.json();
    } catch {
      return new Response('bad json', { status: 400 });
    }

    const text = clip(body.text ?? body.transcript, 1000).trim();
    if (!text) {
      return new Response('missing text', { status: 400 });
    }
    const source = clip(body.source ?? 'voice', 40);

    await mcp.notification({
      method: 'notifications/claude/channel',
      params: {
        content: `August (via voice buddy): ${text}`,
        meta: { text, source },
      },
    });

    return Response.json({ ok: true });
  },
});

process.stderr.write('[voice-channel] listening on http://0.0.0.0:8803\n');
