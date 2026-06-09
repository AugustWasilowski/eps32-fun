# voice-channel

A Claude Code **custom channel** plugin (runs on max, inside the always-on `ss-channels`
session). The buddy server POSTs Whisper transcripts here; the channel forwards each as a
`notifications/claude/channel` event so it surfaces as a turn in Max's session.

Mirrors the proven `~/custom-channels/vikunja-channel` (one-way, token-gated). Port **8803**.

## Install (on max)

This lives alongside the other channels in `~/custom-channels/`. To wire it into the always-on
session, add it as a plugin in `~/custom-channels/.claude-plugin/marketplace.json` (a 4th entry
next to `vikunja-channel` / `ss-alerts-channel` / `ss-chat-channel`), then reload
`ss-channels.service`.

```
cd ~/custom-channels/voice-channel && bun install
# first run prints + saves VOICE_WEBHOOK_TOKEN to ~/.claude/channels/voice-channel/.env
```

Put that token into `server/.env` as `VOICE_CHANNEL_TOKEN`.

## ⚠️ Verify injection first

Per `project_macu_studio_chat_wiring`, channel→turn injection was shelved/broken for
`ss-chat-channel` as of 2026-06-03 (possibly a global regression). `vikunja-channel` is the
documented-working path this mirrors. **Before wiring the device end-to-end**, POST a test
transcript and confirm it surfaces as a turn in `tmux attach -t ss-channels`:

```
curl -s -X POST http://localhost:8803/ \
  -H "X-Webhook-Token: $VOICE_WEBHOOK_TOKEN" \
  -H 'Content-Type: application/json' \
  -d '{"text":"hello from the voice buddy test","source":"voice"}'
```

If it does NOT surface, fall back to having the buddy server write transcripts to a file/queue
Max polls, or post to the Second Shift chat tile.
