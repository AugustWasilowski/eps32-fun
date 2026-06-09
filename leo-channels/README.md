# leo-channels — Leo's voice-buddy channel (Windows Claude Code)

Local Claude Code marketplace + plugin so the ESP32 buddy's **Leo mode** reaches an
always-on Claude Code session ("Leo") on August's Windows PC. Node port of max's
bun-based `ss-chat-channel`.

## Flow
```
buddy (Leo mode) → max /transcribe (X-Buddy-Target: leo) → Whisper
  → max _deliver(leo) → POST http://<windows-ip>:8804/  (leo-channel)
  → notifications/claude/channel → surfaces in Leo's session
  → Leo calls leo_chat_reply → POST max:8810/chat_reply → buddy e-paper "Leo:" line
```

## One-time setup on the Windows PC
1. Copy this `leo-channels/` dir somewhere stable (used: `C:\Users\augus\leo-channels`).
2. `cd leo-channel && npm install`  (pulls `@modelcontextprotocol/sdk`; needs Node 18+).
3. Create `~/.claude/channels/leo-channel/.env` (gitignored) with:
   ```
   LEO_CHANNEL_TOKEN=<shared secret, also in max server/.env LEO_CHANNEL_TOKEN>
   LEO_CHANNEL_PORT=8804
   LEO_REPLY_URL=http://<max-ip>:8810/chat_reply
   ```
4. Register + install the plugin:
   ```
   claude plugin marketplace add C:\Users\augus\leo-channels
   claude plugin install leo-channel@leo-channels
   ```
5. **Allowlist the channel** (managed settings). ⚠️ On Windows (Claude Code ≥ v2.1.75)
   this MUST be at **`C:\Program Files\ClaudeCode\managed-settings.json`** (admin) —
   the old `C:\ProgramData\ClaudeCode\` path is no longer read:
   ```json
   { "channelsEnabled": true,
     "allowedChannelPlugins": [ { "plugin": "leo-channel", "marketplace": "leo-channels" } ] }
   ```
   It must exist **before** the session starts; restart to apply. Without it the session
   shows "leo-channel … not on the approved channels allowlist" and events never surface.
6. Open Windows Firewall for the channel port: inbound TCP 8804 (so max can reach it).
7. Launch the always-on session (visible terminal, keep open):
   ```
   claude --channels plugin:leo-channel@leo-channels
   ```

## max side
`server/.env`: `LEO_CHANNEL_URL=http://<windows-ip>:8804/`, `LEO_CHANNEL_TOKEN=<same>`,
`LEO_REPLY_URL=http://<max-ip>:8810/chat_reply`. `buddy_server` `/transcribe` routes
`X-Buddy-Target: leo` here; `/chat_reply` accepts the Leo token.
