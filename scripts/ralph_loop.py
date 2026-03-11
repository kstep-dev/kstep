#!/usr/bin/env python3
"""
Ralph Loop: Autonomous kSTEP bug reproduction agent.

Each iteration starts a FRESH copilot agent that:
  1. Reads TODO.md to find pending bugs
  2. Picks the most promising one
  3. Attempts to reproduce it (checkout, write driver, build, run, verify)
  4. Updates TODO.md inline with results (status, driver, attempts, comments)
  5. Git commits + pushes

TODO.md IS the state — no separate state file needed.

Line format in TODO.md:
  - [ ] `hash` title — [`file.md`](bugs/file.md)                                ← pending
  - [ ] `hash` title — [`file.md`](bugs/file.md) <!-- attempts:2 -->             ← tried
  - [x] `hash` title — [`file.md`](bugs/file.md) <!-- driver:foo attempts:1 -->  ← done
  - [-] `hash` title — [`file.md`](bugs/file.md) <!-- skipped:reason attempts:3 --> ← skipped
"""

import argparse
import asyncio
import logging
import re
import subprocess
from pathlib import Path

PROJ_DIR = Path(__file__).resolve().parent.parent
TODO_FILE = PROJ_DIR / "TODO.md"
BUGS_DIR = PROJ_DIR / "data" / "bugs"

DEFAULT_TIME_BUDGET = 10
DEFAULT_MAX_ITERS = 400
DEFAULT_PARALLEL = 8

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s",
    datefmt="%H:%M:%S",
)
log = logging.getLogger("ralph")

# ─── Parse TODO.md ────────────────────────────────────────────────────

LINE_RE = re.compile(
    r"^- \[(?P<status>[ x\-])\] "
    r"`(?P<hash>[0-9a-f]+)` "
    r"(?P<title>.+?) — "
    r"\[`(?P<file>.+?\.md)`\]"
)
META_RE = re.compile(r"<!--\s*(?P<meta>.+?)\s*-->")


def parse_todo() -> list[dict]:
    entries = []
    for line in TODO_FILE.read_text().splitlines():
        m = LINE_RE.match(line)
        if not m:
            continue
        status_char = m.group("status")
        status = {"x": "done", "-": "skipped", " ": "pending"}[status_char]
        meta = {}
        mm = META_RE.search(line)
        if mm:
            for part in mm.group("meta").split():
                if ":" in part:
                    k, v = part.split(":", 1)
                    meta[k] = v
        entries.append({
            "hash": m.group("hash"),
            "title": m.group("title"),
            "file": m.group("file"),
            "status": status,
            "attempts": int(meta.get("attempts", 0)),
            "driver": meta.get("driver"),
        })
    return entries


def count_states(entries: list[dict]) -> dict:
    c = {"pending": 0, "done": 0, "skipped": 0}
    for e in entries:
        c[e["status"]] = c.get(e["status"], 0) + 1
    return c


# ─── Prompt ───────────────────────────────────────────────────────────


def build_prompt(bug: dict, entries: list[dict], time_budget: int) -> str:
    counts = count_states(entries)
    return f"""\
You are an autonomous kSTEP bug reproduction agent running inside a Ralph loop.
Each iteration you start with a FRESH context.  All state lives in TODO.md.

═══ CURRENT PROGRESS ═══
  Done:     {counts['done']}
  Skipped:  {counts['skipped']}
  Pending:  {counts['pending']}
  Total:    {len(entries)}
  Time budget: {time_budget} minutes

═══ YOUR ASSIGNED BUG ═══
  File:     {bug['file']}
  Commit:   {bug['hash']}
  Title:    {bug['title']}
  Attempts: {bug['attempts']}

═══ YOUR TASK ═══

1. Read the bug file: bugs/{bug['file']}
   Pay attention to "## Triggering Conditions" and "## Reproduce Strategy (kSTEP)".

2. REPRODUCE the bug following AGENTS.md:
   a) Read the patch: git -C linux/current show -U32 {bug['hash']}
   b) Checkout buggy kernel: ./checkout_linux.py {bug['hash']}~1 <name>_buggy
   c) Write driver: kmod/drivers/<name>.c
   d) Build: make linux LINUX_NAME=<name>_buggy && make kstep LINUX_NAME=<name>_buggy
   e) Run: ./run.py <name> --linux_name <name>_buggy
   f) Check: cat data/logs/latest.log
   g) If bug triggers, verify the fix:
        ./checkout_linux.py {bug['hash']} <name>_fixed
        make linux LINUX_NAME=<name>_fixed && make kstep LINUX_NAME=<name>_fixed
        ./run.py <name> --linux_name <name>_fixed
   h) Confirm bug is gone on fixed kernel.

3. UPDATE TODO.md — find and edit the line containing `{bug['hash'][:12]}`:

   On SUCCESS (bug triggers on buggy, not on fixed):
     Change "- [ ]" to "- [x]" and append/update the HTML comment:
     - [x] `{bug['hash'][:12]}` ... <!-- driver:<name> attempts:<N> -->

   On SKIP (cannot reproduce with kSTEP, or out of time):
     Change "- [ ]" to "- [-]" and append/update the HTML comment:
     - [-] `{bug['hash'][:12]}` ... <!-- skipped:<reason> attempts:<N> -->

   If still trying (ran out of time but want to retry later):
     Keep "- [ ]" but update the attempts count:
     - [ ] `{bug['hash'][:12]}` ... <!-- attempts:<N> -->

   Also update the header counter: **Total: X/400 reproduced**

4. If successful, add the bug to reproduce.py (follow existing entries).

5. Git commit and push:
     git add -A
     git commit -m "reproduce: <driver_name> ({bug['hash'][:12]})

   Co-authored-by: Copilot <223556219+Copilot@users.noreply.github.com>"
     git push

═══ IMPORTANT ═══
- Work ONLY on the assigned bug above.
- Stay within the time budget — skip if you're running out of time.
- CPU 0 is reserved — use CPU 1+ for tasks.
- Always commit and push before finishing.
"""


# ─── Agent execution ─────────────────────────────────────────────────


async def run_one_agent(bug: dict, entries: list[dict], time_budget: int,
                        semaphore: asyncio.Semaphore):
    """Run a single agent on an assigned bug, saving output to a log file."""
    async with semaphore:
        prompt = build_prompt(bug, entries, time_budget)
        timeout_sec = (time_budget + 3) * 60
        short = bug['hash'][:12]

        BUGS_DIR.mkdir(parents=True, exist_ok=True)
        log_path = BUGS_DIR / bug['file'].replace('.md', '.log')

        log.info("🚀 Starting agent for %s (%s)  →  %s", short, bug['title'][:50], log_path)
        try:
            with open(log_path, "w") as log_file:
                proc = await asyncio.create_subprocess_exec(
                    "copilot", "-p", prompt, "--allow-all", "--no-color",
                    stdout=log_file,
                    stderr=asyncio.subprocess.STDOUT,
                    cwd=str(PROJ_DIR),
                )
                await asyncio.wait_for(proc.wait(), timeout=timeout_sec)
            if proc.returncode == 0:
                log.info("✅ Agent for %s exited normally", short)
            else:
                log.warning("⚠️  Agent for %s exited with code %d", short, proc.returncode)
        except asyncio.TimeoutError:
            log.warning("⏰ Agent for %s timed out — killing", short)
            proc.kill()
            await proc.wait()


# ─── Auto-commit + push fallback ─────────────────────────────────────


def auto_commit_and_push():
    result = subprocess.run(
        ["git", "status", "--porcelain"],
        capture_output=True, text=True, cwd=str(PROJ_DIR),
    )
    if not result.stdout.strip():
        return

    msg = (
        "ralph-loop: save iteration progress\n\n"
        "Co-authored-by: Copilot <223556219+Copilot@users.noreply.github.com>"
    )
    subprocess.run(["git", "add", "-A"], cwd=str(PROJ_DIR))
    result = subprocess.run(
        ["git", "commit", "-m", msg],
        capture_output=True, text=True, cwd=str(PROJ_DIR),
    )
    if result.returncode == 0:
        log.info("📦 Auto-committed leftover changes")
        push = subprocess.run(
            ["git", "push"],
            capture_output=True, text=True, cwd=str(PROJ_DIR),
        )
        if push.returncode == 0:
            log.info("🚀 Pushed to remote")
        else:
            log.warning("Push failed: %s", push.stderr.strip()[:200])


# ─── Main loop ────────────────────────────────────────────────────────


async def main_loop(args):
    semaphore = asyncio.Semaphore(args.parallel)
    wave = 0

    while True:
        entries = parse_todo()
        counts = count_states(entries)
        pending = [e for e in entries if e["status"] == "pending"]

        if not pending:
            log.info("🎉 No pending bugs remaining!")
            break

        wave += 1
        batch = pending[:args.parallel]

        log.info(
            "══════════ Wave %d  |  %d agents  |  done=%d  skipped=%d  pending=%d ══════════",
            wave, len(batch), counts["done"], counts["skipped"], counts["pending"],
        )

        tasks = [
            run_one_agent(bug, entries, args.time_budget, semaphore)
            for bug in batch
        ]
        await asyncio.gather(*tasks)

        # Commit + push anything agents left behind
        auto_commit_and_push()

        after = count_states(parse_todo())
        new_done = after["done"] - counts["done"]
        new_skipped = after["skipped"] - counts["skipped"]
        log.info("Wave %d results: +%d reproduced, +%d skipped", wave, new_done, new_skipped)

        if wave >= args.max_iterations:
            log.info("Reached max iterations (%d)", args.max_iterations)
            break

    final = count_states(parse_todo())
    log.info("═══════════════════════════════════════════")
    log.info("FINAL: done=%d  skipped=%d  pending=%d",
             final["done"], final["skipped"], final["pending"])


# ─── CLI ──────────────────────────────────────────────────────────────


def show_status():
    entries = parse_todo()
    counts = count_states(entries)
    print(f"Done:     {counts['done']}")
    print(f"Skipped:  {counts['skipped']}")
    print(f"Pending:  {counts['pending']}")
    print(f"Total:    {len(entries)}")
    done = [e for e in entries if e["status"] == "done"]
    if done:
        print("\nReproduced:")
        for e in done:
            print(f"  ✅ {e['hash'][:12]}  {e['title']}  (driver: {e.get('driver', '?')})")
    skipped = [e for e in entries if e["status"] == "skipped"]
    if skipped:
        print(f"\nSkipped ({len(skipped)}):")
        for e in skipped[:20]:
            print(f"  ⏭️  {e['hash'][:12]}  {e['title']}")


def main():
    parser = argparse.ArgumentParser(
        description="Ralph Loop: Autonomous kSTEP bug reproduction"
    )
    sub = parser.add_subparsers(dest="command", required=True)

    run_p = sub.add_parser("run", help="Start/resume the Ralph loop")
    run_p.add_argument(
        "--time-budget", "-t", type=int, default=DEFAULT_TIME_BUDGET,
        help=f"Minutes per bug attempt (default: {DEFAULT_TIME_BUDGET})",
    )
    run_p.add_argument(
        "--max-iterations", "-n", type=int, default=DEFAULT_MAX_ITERS,
        help=f"Max waves (default: {DEFAULT_MAX_ITERS})",
    )
    run_p.add_argument(
        "--parallel", "-p", type=int, default=DEFAULT_PARALLEL,
        help=f"Concurrent agents per wave (default: {DEFAULT_PARALLEL})",
    )

    sub.add_parser("status", help="Show current progress")
    args = parser.parse_args()

    if args.command == "status":
        show_status()
    elif args.command == "run":
        asyncio.run(main_loop(args))


if __name__ == "__main__":
    main()
