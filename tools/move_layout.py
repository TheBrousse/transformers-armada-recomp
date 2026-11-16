#!/usr/bin/env python3
"""(TODO 279, docs/INSTALL_LAYOUT_PLAN.md section 3) Moves a player's Linux install to the new folder layout, once:
the player's own things out of the checkout into the install folder, a backup first, nothing deleted. The Linux
counterpart of tools/move_layout.ps1, step for step.

Started by build.sh at the end of a good build (tools/install_layout.sh: tfa_layout_move), in an install only: the
setup program (tfa-setup) or its .setup_unfinished mark in the folder above the checkout. A checkout cloned and set up
by hand is never moved by a build or an update; its owner can run this by hand to move it:
    python3 tools/move_layout.py --game-folder . --root ..
The game must be closed (the updater waited for it); when it runs this does nothing and the next build tries again.

The new places, all in the install folder (<install>):
  settings.ini                  from the checkout's settings.ini
  saves/mc0, saves/mc1          the memory cards, from game/mc0, game/mc1
  saves/states/                 save states: quick.ps2state and states/*.ps2state (the updater's own files in states/
                                stay: update.sh and the setup program write them there)
  saves/backups, saves/exports  F1 > Saves' folders, from Saves/Backups, Saves/Exports (and the rest of Saves/)
  logs/                         the newest game logs (runs/play_*), copied
  data/game                     the disc's files, moved (a rename in the same folder tree: instant)
  data/movies_hd                the HD movies, moved
  layout.json                   {"layout": 2}, written once the copies are checked and before the big folders move:
                                the game uses each new place only once it is there
The HD textures and the mods stay in the checkout, as on Windows: the tools that make them write them there, and the
game finds them there in both layouts.

Safety, in this order:
  1. A journal (<install>/layout_move.json: each step and its state), so an interrupted move carries on. Every write
     of it, of layout.json and of each copied file goes to a temporary file first, is synced to the disk and then put
     in place by a rename: a file is either the old one or the whole new one, even if the PC goes off.
  2. A backup of the saves, the settings and the states in <install>/saves/backups/before_layout2_<date>/, every file
     checked by its hash. When it cannot be made (no room, no rights) NOTHING moves and the install plays on as it was.
  3. Saves, settings and states COPIED and checked by hash; the originals stay where they were. Until layout.json is
     written, a run that carries on an interrupted move copies them all again (only what changed is copied), so a
     save made in the meanwhile - the game still used the old places then - is not left behind.
  4. layout.json, then the big folders renamed. The old memory cards, now inside data/game, are renamed mc0.moved and
     mc1.moved (two checked copies of them exist: saves/ and the backup) so they cannot be taken for the cards in use.

No absolute path is written anywhere (layout.json holds only the number): a moved install can be copied whole to
another folder or PC (section 2.4). A settings.ini already in the install folder is merged with the checkout's: the
checkout's file is the base, the install folder's values win (both are in the backup).

Exit code 0: moved, carried on to the end, or nothing to do; 1: not moved (said why), the install plays on as it was.
TF_LAYOUT_TEST_STOP=<journal|backup|copies|layout|moves> (tests/layout_move_tests.sh only): stop dead at that point, as
a killed update would.
"""

import argparse
import datetime
import hashlib
import json
import os
import re
import shutil
import sys

JOURNAL = "layout_move.json"
TMP = ".layout_move_tmp"  # (in the install folder: the same file system as every place a file is put)
EXES = ("SLUS_206.68", "SLES_523.88")


def say(message):
    print(f"  [layout] {message}", flush=True)


def stop_point(name):
    if os.environ.get("TF_LAYOUT_TEST_STOP") == name:
        sys.stdout.flush()
        os._exit(9)


def ids(path):
    try:
        s = os.stat(path)
        return (s.st_dev, s.st_ino)
    except OSError:
        return None


def game_running(game_folder):
    """The game started from this checkout: its executable, or (an older build replaced since) a TransformersRecomp
    whose working folder is this game folder (the game makes its folder the current one). Through /proc, which a
    distrobox container shares with the system."""
    exe = ids(os.path.join(game_folder, "out", "build", "transformers", "TransformersRecomp"))
    folder = ids(game_folder)
    for pid in os.listdir("/proc"):
        if not pid.isdigit():
            continue
        try:
            link = os.readlink(f"/proc/{pid}/exe")
        except OSError:
            continue
        if not os.path.basename(link).startswith("TransformersRecomp"):
            continue
        if (exe and ids(f"/proc/{pid}/exe") == exe) or (folder and ids(f"/proc/{pid}/cwd") == folder):
            return True
    return False


def layout_of(root):
    try:
        text = open(os.path.join(root, "layout.json"), encoding="utf-8", errors="replace").read()
    except OSError:
        return 1
    m = re.search(r'"layout"\s*:\s*(\d+)', text)
    return int(m.group(1)) if m else 1


def sync_dir(path):
    try:
        fd = os.open(path, os.O_RDONLY)
        try:
            os.fsync(fd)
        finally:
            os.close(fd)
    except OSError:
        pass


def write_file(path, text, root):
    """Written whole or not at all: a temporary file, synced, renamed into place."""
    tmp_dir = os.path.join(root, TMP)
    os.makedirs(tmp_dir, exist_ok=True)
    tmp = os.path.join(tmp_dir, os.path.basename(path) + ".new")
    with open(tmp, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
        f.flush()
        os.fsync(f.fileno())
    os.makedirs(os.path.dirname(path), exist_ok=True)
    os.replace(tmp, path)
    sync_dir(os.path.dirname(path))


def sha(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def files_under(path):
    """The files under a path (a file or a folder), relative to it ("" for the file itself)."""
    if not os.path.isdir(path):
        return [""]
    found = []
    for here, _dirs, files in os.walk(path):
        for name in files:
            found.append(os.path.relpath(os.path.join(here, name), path))
    return sorted(found)


def size_of(path):
    return sum(os.path.getsize(os.path.join(path, rel) if rel else path) for rel in files_under(path))


def copy_checked(src, dst, root):
    """Copies a file or a folder and checks every file by its hash; true when every file arrived intact. A file that is
    already there with the same contents is left as it is."""
    tmp_dir = os.path.join(root, TMP)
    os.makedirs(tmp_dir, exist_ok=True)
    for rel in files_under(src):
        s = os.path.join(src, rel) if rel else src
        d = os.path.join(dst, rel) if rel else dst
        want = sha(s)
        if not (os.path.isfile(d) and os.path.getsize(d) == os.path.getsize(s) and sha(d) == want):
            os.makedirs(os.path.dirname(d), exist_ok=True)
            tmp = os.path.join(tmp_dir, "copy")
            shutil.copyfile(s, tmp)
            try:
                shutil.copystat(s, tmp)
            except OSError:
                pass
            with open(tmp, "rb+") as f:
                os.fsync(f.fileno())
            os.replace(tmp, d)
            sync_dir(os.path.dirname(d))
        if sha(d) != want:
            return False
    if os.path.isdir(src):
        os.makedirs(dst, exist_ok=True)
    return True


def merge_settings(base, over):
    """settings.ini: the checkout's lines as they are, each KEY=value of the install folder's file put over the same
    key (or added at the end). Comments and blank lines of the checkout's file are kept."""
    key = re.compile(r"^\s*([^#;=\s][^=]*?)\s*=")

    def key_of(line):
        m = key.match(line)
        return m.group(1).upper() if m else None

    lines = open(base, encoding="utf-8", errors="replace").read().splitlines()
    for line in open(over, encoding="utf-8", errors="replace").read().splitlines():
        k = key_of(line)
        if not k:
            continue
        at = -1
        for i, mine in enumerate(lines):
            if key_of(mine) == k:
                at = i
        if at >= 0:
            lines[at] = line
        else:
            lines.append(line)
    return "\n".join(lines) + "\n"


def plan(game_folder):
    """What there is to move: (kind, from - relative to the checkout, to - relative to the install folder).
    copy: copied and checked (the original stays); move: renamed (big folders)."""
    p = [("copy", f"game/{card}", f"saves/{card}") for card in ("mc0", "mc1")]
    p.append(("copy", "settings.ini", "settings.ini"))
    p.append(("copy", "quick.ps2state", "saves/states/quick.ps2state"))
    states = os.path.join(game_folder, "states")
    if os.path.isdir(states):
        # (not the updater's own files: update_resume.*, update_result.txt, update_kept.txt and update.sh's hidden
        # .play_after_update stay in states/)
        for name in sorted(os.listdir(states)):
            if os.path.isfile(os.path.join(states, name)) and not name.startswith(("update_", ".")):
                p.append(("copy", f"states/{name}", f"saves/states/{name}"))
    saves = os.path.join(game_folder, "Saves")
    if os.path.isdir(saves):
        names = {"Backups": "saves/backups", "Exports": "saves/exports", "README.txt": "saves/README_old.txt"}
        for name in sorted(os.listdir(saves)):
            p.append(("copy", f"Saves/{name}", names.get(name, f"saves/{name}")))
    p.append(("move", "game", "data/game"))
    p.append(("move", "movies_hd", "data/movies_hd"))
    return [s for s in p if os.path.lexists(os.path.join(game_folder, s[1]))]


def main():
    ap = argparse.ArgumentParser(description="Moves a player's install to the new folder layout, once.")
    ap.add_argument("--game-folder", required=True, help="the checkout (or its port/)")
    ap.add_argument("--root", required=True, help="the install folder")
    args = ap.parse_args()
    game_folder = os.path.abspath(args.game_folder)
    root = os.path.abspath(args.root)
    journal_file = os.path.join(root, JOURNAL)

    if game_running(game_folder):
        say("the game is running: the folders are moved at the next update")
        return 0
    journal = None
    if os.path.isfile(journal_file):
        try:
            journal = json.load(open(journal_file, encoding="utf-8"))
        except (OSError, ValueError):
            journal = None
    # (moved and finished: nothing to do; layout.json there but the journal unfinished: an interrupted move carries on)
    if layout_of(root) == 2 and (journal is None or journal.get("finished")):
        return 0
    print("\n==> Moving your saves, settings and the disc's files into the install folder (once)", flush=True)
    stamp = datetime.datetime.now().strftime("%Y-%m-%d_%H%M%S")
    checkout_name = os.path.relpath(game_folder, root)  # (transformers-armada-recomp, or .../port)

    def save_journal():
        write_file(journal_file, json.dumps(journal, indent=2) + "\n", root)

    try:
        shutil.rmtree(os.path.join(root, TMP), ignore_errors=True)
        # --- 1. The journal: started, or carried on ---------------------------------------------------------------
        if journal is not None:
            say(f"carrying on the move started {journal.get('started')}")
            if not journal.get("layout_written"):
                # (what is there now, added: a save state made while the game still used the old places)
                have = {s["from"] for s in journal["steps"]}
                for kind, src, dst in plan(game_folder):
                    if src not in have:
                        journal["steps"].append({"kind": kind, "from": src, "to": dst, "state": "todo"})
        else:
            journal = {
                "started": stamp,
                "backup": f"saves/backups/before_layout2_{stamp}",
                "backup_done": False,
                "layout_written": False,
                # (a settings.ini in the install folder before the move - not one this move made - is merged, below)
                "install_settings": os.path.isfile(os.path.join(root, "settings.ini")),
                "steps": [{"kind": k, "from": s, "to": d, "state": "todo"} for k, s, d in plan(game_folder)],
            }
        # (copy steps first, then the moves, whatever order they were added in)
        journal["steps"].sort(key=lambda s: s["kind"] != "copy")
        save_journal()
        stop_point("journal")
        copies = [s for s in journal["steps"] if s["kind"] == "copy"]
        backup = os.path.join(root, journal["backup"])

        # --- 2. The backup: the saves, the settings, the states - checked, or nothing moves ------------------------
        if not journal.get("backup_done"):
            need = sum(size_of(os.path.join(game_folder, s["from"])) for s in copies if os.path.lexists(os.path.join(game_folder, s["from"])))
            free = shutil.disk_usage(root).free
            # (twice what is copied - the backup and the copy - and 200 MB to spare)
            if free < 2 * need + 200 * 1024 * 1024:
                raise RuntimeError(f"not enough free space for the backup ({round(need / 1048576)} MB, twice)")
            for s in copies:
                src = os.path.join(game_folder, s["from"])
                if os.path.lexists(src) and not copy_checked(src, os.path.join(backup, s["from"]), root):
                    raise RuntimeError(f"the backup of {s['from']} did not check out")
            theirs = os.path.join(backup, "settings_install_folder.ini")
            if journal.get("install_settings") and os.path.isfile(os.path.join(root, "settings.ini")) and not os.path.isfile(theirs):
                if not copy_checked(os.path.join(root, "settings.ini"), theirs, root):
                    raise RuntimeError("the backup of the install folder's settings.ini did not check out")
            os.makedirs(backup, exist_ok=True)
            write_file(os.path.join(backup, "README.txt"),
                       f"A copy of your saves, settings and save states as they were before the game's folders were tidied up on {journal['started']}.\n"
                       f"They were in the folder {checkout_name} (game/mc0, game/mc1, settings.ini, quick.ps2state, states, Saves).\n"
                       "Nothing needs to be done with it: keep it until you are sure your saves are fine, then delete it if you need the space.\n", root)
            journal["backup_done"] = True
            save_journal()
            say(f"backup made: {journal['backup']}")
        stop_point("backup")

        # --- 3. Saves, settings, states: copied and checked (the originals stay) -----------------------------------
        # (until layout.json is written every step is copied again: only what changed since is copied)
        for s in copies:
            if s["state"] == "done" and journal.get("layout_written"):
                continue
            src = os.path.join(game_folder, s["from"])
            dst = os.path.join(root, s["to"])
            if not os.path.lexists(src):
                s["state"] = "done"
                continue
            if s["to"] == "settings.ini":
                # (one there already before the move, kept in the backup: merged - its values win, the player's other
                # settings stay)
                theirs = os.path.join(backup, "settings_install_folder.ini")
                if os.path.isfile(theirs):
                    write_file(dst, merge_settings(src, theirs), root)
                    s["state"] = "done"
                    save_journal()
                    continue
            if not copy_checked(src, dst, root):
                raise RuntimeError(f"the copy of {s['from']} did not check out")
            s["state"] = "done"
            save_journal()
        stop_point("copies")

        # --- 4. layout.json: from now on the game and the scripts use the new places that are there ---------------
        # (written before the big folders move: each place is used only once it is there - install_layout.cpp,
        # tools/install_layout.sh - so a move stopped half-way leaves every item findable, in its old place or its new)
        # (saves/mc0 made even when there was no card yet: the game's new saves must land there, not in data/game)
        for d in ("saves/mc0", "saves/states", "data", "logs"):
            os.makedirs(os.path.join(root, d), exist_ok=True)
        # (a fresh install has no settings.ini yet: one made from the example here, so every way of starting the game -
        # the setup program, play.sh, "Transformers Armada" - finds the same file in the install folder)
        root_settings = os.path.join(root, "settings.ini")
        if not os.path.isfile(root_settings):
            for example in ("overlay/settings.example.ini", "settings.example.ini"):
                if os.path.isfile(os.path.join(game_folder, example)):
                    copy_checked(os.path.join(game_folder, example), root_settings, root)
                    break
        if layout_of(root) != 2:
            write_file(os.path.join(root, "layout.json"), '{"layout": 2}\n', root)
        journal["layout_written"] = True
        save_journal()
        stop_point("layout")

        # --- 5. The big folders: renamed (the same folder tree, instant) -------------------------------------------
        for s in journal["steps"]:
            if s["kind"] != "move" or s["state"] == "done":
                continue
            src = os.path.join(game_folder, s["from"])
            dst = os.path.join(root, s["to"])
            if os.path.lexists(src) and not os.path.lexists(dst):
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                os.rename(src, dst)  # (another file system: an error, and the folder stays where it is, still found)
                sync_dir(os.path.dirname(src))
                sync_dir(os.path.dirname(dst))
            elif os.path.lexists(src) and os.path.lexists(dst):
                raise RuntimeError(f"both {src} and {dst} exist: left as they are")
            s["state"] = "done"
            save_journal()
            stop_point("moves")
        # (the old cards came along inside data/game: renamed, so they cannot be taken for the cards in use)
        for card in ("mc0", "mc1"):
            old = os.path.join(root, "data", "game", card)
            if os.path.isdir(old) and os.path.isdir(os.path.join(root, "saves", card)) and not os.path.lexists(old + ".moved"):
                os.rename(old, old + ".moved")
        if os.path.isdir(os.path.join(root, "data", "game")):
            write_file(os.path.join(root, "data", "game", "README.txt"),
                       "The game's files from your disc. Do not share this folder.\n"
                       "mc0.moved and mc1.moved are your memory cards as they were before the move: the game uses saves/mc0 and saves/mc1 now.\n", root)

        # --- 6. Logs: the newest game logs copied into logs/ --------------------------------------------------------
        logs = os.path.join(root, "logs")
        runs = os.path.join(game_folder, "runs")
        if os.path.isdir(runs):
            played = [os.path.join(runs, n) for n in os.listdir(runs) if n.startswith("play_") and os.path.isfile(os.path.join(runs, n))]
            for path in sorted(played, key=os.path.getmtime, reverse=True)[:20]:
                try:
                    shutil.copy2(path, logs)
                except OSError:
                    pass
        write_file(os.path.join(root, "saves", "README.txt"),
                   "Your saves.\n\n"
                   "mc0 and mc1: your memory card saves (the same files a PlayStation 2 writes to a memory card).\n"
                   "states: save states (F6 / F9 and the named ones). A save state works only with the version of the game that made it.\n"
                   "backups, exports: what F1 > Saves makes.\n\n"
                   "Back up: copy this whole saves folder somewhere safe. Never post save files or save states in public: they contain game data.\n", root)

        journal["finished"] = datetime.datetime.now().strftime("%Y-%m-%d_%H%M%S")
        save_journal()
        shutil.rmtree(os.path.join(root, TMP), ignore_errors=True)
        say(f"done: saves in {os.path.join(root, 'saves')}, settings in {root_settings}, the disc's files in {os.path.join(root, 'data', 'game')}")
        say(f"the old saves and settings stay in the checkout as they were, and a backup is in {journal['backup']}")
        return 0
    except Exception as error:  # noqa: BLE001 - whatever stopped it, the install plays on as it was
        # (before layout.json nothing uses the new places; the journal carries on next time)
        if journal and journal.get("layout_written"):
            say(f"the move stopped before its end: {error}. Your saves and settings are in their new places already; "
                "what was not moved stays where it was and is still found. The next update carries on.")
        else:
            say(f"the folders were not moved: {error}. The game plays on as before; the next update tries again.")
        return 1


if __name__ == "__main__":
    sys.exit(main())
