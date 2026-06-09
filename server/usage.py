"""Usage + context metrics for the /display endpoint.

- token_usage_today(): today's Claude token total + cost, via `ccusage` if available,
  else by summing usage fields across today's session JSONL files.
- context_pct(): how full the always-on ss-channels session's context window is, read
  from the latest assistant message's usage in its transcript JSONL.

Both degrade gracefully to None on any error so /display never 500s.

Env:
  CLAUDE_DIR        default ~/.claude
  SS_PROJECT_DIR    default "-home-mayorawesome"  (the ss-channels project folder name)
  SS_SESSION_JSONL  optional explicit path to the ss-channels session .jsonl
                    (otherwise: most-recently-modified .jsonl in the project dir)
  CONTEXT_WINDOW    default 200000  (set to 1000000 if Max runs the 1M-context model)
"""
import json
import os
import subprocess
from pathlib import Path

CLAUDE_DIR = Path(os.environ.get("CLAUDE_DIR", Path.home() / ".claude"))
SS_PROJECT_DIR = os.environ.get("SS_PROJECT_DIR", "-home-mayorawesome")
CONTEXT_WINDOW = int(os.environ.get("CONTEXT_WINDOW", "200000"))


def _project_path() -> Path:
    return CLAUDE_DIR / "projects" / SS_PROJECT_DIR


def _ss_session_jsonl() -> Path | None:
    explicit = os.environ.get("SS_SESSION_JSONL")
    if explicit:
        p = Path(explicit)
        return p if p.exists() else None
    proj = _project_path()
    if not proj.is_dir():
        return None
    jsonls = list(proj.glob("**/*.jsonl"))
    if not jsonls:
        return None
    return max(jsonls, key=lambda p: p.stat().st_mtime)


def context_pct() -> float | None:
    """Percent of the context window used by the latest assistant turn (0-100)."""
    path = _ss_session_jsonl()
    if not path:
        return None
    last_usage = None
    try:
        with path.open("r", encoding="utf-8", errors="ignore") as fh:
            for line in fh:
                line = line.strip()
                if not line or '"usage"' not in line:
                    continue
                try:
                    obj = json.loads(line)
                except json.JSONDecodeError:
                    continue
                msg = obj.get("message") or obj
                usage = msg.get("usage") if isinstance(msg, dict) else None
                if usage:
                    last_usage = usage
    except OSError:
        return None
    if not last_usage:
        return None
    used = (
        last_usage.get("input_tokens", 0)
        + last_usage.get("cache_read_input_tokens", 0)
        + last_usage.get("cache_creation_input_tokens", 0)
    )
    return round(100.0 * used / CONTEXT_WINDOW, 1)


def token_usage_today() -> dict:
    """{"tokens": int|None, "cost": float|None} for today."""
    # Preferred: ccusage (https://github.com/ryoppippi/ccusage)
    try:
        out = subprocess.run(
            ["npx", "-y", "ccusage", "daily", "--json", "--since", "today"],
            capture_output=True,
            text=True,
            timeout=90,
        )
        if out.returncode == 0 and out.stdout.strip():
            data = json.loads(out.stdout)
            rows = data.get("daily") or data.get("data") or []
            if rows:
                row = rows[-1]
                tokens = (
                    row.get("totalTokens")
                    or (row.get("inputTokens", 0) + row.get("outputTokens", 0))
                    or None
                )
                cost = row.get("totalCost") or row.get("costUSD")
                return {"tokens": tokens, "cost": cost}
    except Exception:  # noqa: BLE001
        pass

    # Fallback: sum usage across today's JSONL (best-effort, tokens only).
    try:
        import datetime as _dt

        today = _dt.date.today().isoformat()
        total = 0
        proj = CLAUDE_DIR / "projects"
        for jsonl in proj.glob("**/*.jsonl"):
            if _dt.date.fromtimestamp(jsonl.stat().st_mtime).isoformat() != today:
                continue
            with jsonl.open("r", encoding="utf-8", errors="ignore") as fh:
                for line in fh:
                    if '"usage"' not in line:
                        continue
                    try:
                        obj = json.loads(line)
                    except json.JSONDecodeError:
                        continue
                    msg = obj.get("message") or obj
                    u = msg.get("usage") if isinstance(msg, dict) else None
                    if u:
                        total += u.get("input_tokens", 0) + u.get("output_tokens", 0)
        return {"tokens": total or None, "cost": None}
    except Exception:  # noqa: BLE001
        return {"tokens": None, "cost": None}
