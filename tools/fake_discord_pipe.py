#!/usr/bin/env python3
"""A stand-in for the Discord app's IPC pipe (tests of the Discord status, transformers/src/discord_presence.h).

It serves \\\\.\\pipe\\<name> as Discord does - a frame is a little-endian u32 opcode, a little-endian u32 length and that
much JSON; the handshake (op 0) gets a READY frame, or with --reject an error close (op 2, as for an unknown
application id) - and writes every frame the game sends as one JSON line to --out:
{"time": <s since start>, "op": <n>, "data": <the frame's JSON>}, and {"event": "connected"/"disconnected"}.
After a disconnect it waits for the next connection, until --seconds have passed.

The game uses it with TF_DISCORD_PIPE=<name> (then it opens only that pipe, never the real Discord's).
Windows only (ctypes, no packages). Not part of the game.

    python tools/fake_discord_pipe.py --name tf-discord-test --out runs/discord_frames.jsonl --seconds 600
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import json
import struct
import sys
import time

kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
kernel32.CreateNamedPipeW.restype = wt.HANDLE
kernel32.CreateNamedPipeW.argtypes = [wt.LPCWSTR, wt.DWORD, wt.DWORD, wt.DWORD, wt.DWORD, wt.DWORD, wt.DWORD, wt.LPVOID]
kernel32.ConnectNamedPipe.argtypes = [wt.HANDLE, wt.LPVOID]
kernel32.ReadFile.argtypes = [wt.HANDLE, wt.LPVOID, wt.DWORD, ctypes.POINTER(wt.DWORD), wt.LPVOID]
kernel32.WriteFile.argtypes = [wt.HANDLE, wt.LPCVOID, wt.DWORD, ctypes.POINTER(wt.DWORD), wt.LPVOID]
kernel32.PeekNamedPipe.argtypes = [wt.HANDLE, wt.LPVOID, wt.DWORD, wt.LPVOID, ctypes.POINTER(wt.DWORD), wt.LPVOID]
kernel32.DisconnectNamedPipe.argtypes = [wt.HANDLE]
kernel32.CloseHandle.argtypes = [wt.HANDLE]

PIPE_ACCESS_DUPLEX = 0x3
PIPE_TYPE_BYTE_WAIT = 0x0
PIPE_NOWAIT = 0x1
INVALID = wt.HANDLE(-1).value
ERROR_PIPE_CONNECTED = 535


def read_exact(pipe, n, deadline):
    data = b""
    while len(data) < n:
        avail = wt.DWORD(0)
        if not kernel32.PeekNamedPipe(pipe, None, 0, None, ctypes.byref(avail), None):
            return None  # (the game closed its end)
        if avail.value == 0:
            if time.monotonic() > deadline:
                return b""
            time.sleep(0.02)
            continue
        buf = ctypes.create_string_buffer(n - len(data))
        got = wt.DWORD(0)
        if not kernel32.ReadFile(pipe, buf, n - len(data), ctypes.byref(got), None):
            return None
        data += buf.raw[: got.value]
    return data


def write_frame(pipe, op, obj):
    payload = json.dumps(obj).encode()
    frame = struct.pack("<II", op, len(payload)) + payload
    done = wt.DWORD(0)
    kernel32.WriteFile(pipe, frame, len(frame), ctypes.byref(done), None)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--name", default="tf-discord-test")
    ap.add_argument("--out", required=True)
    ap.add_argument("--seconds", type=float, default=600)
    ap.add_argument("--reject", action="store_true", help="refuse the handshake as Discord does an unknown id")
    args = ap.parse_args()
    start = time.monotonic()
    end = start + args.seconds
    path = "\\\\.\\pipe\\" + args.name
    with open(args.out, "a", encoding="utf-8") as out:
        def log(**kw):
            kw["time"] = round(time.monotonic() - start, 2)
            out.write(json.dumps(kw) + "\n")
            out.flush()

        log(event="listening", pipe=path)
        while time.monotonic() < end:
            # (non-blocking connect, so --seconds also ends a wait for a game that never comes)
            pipe = kernel32.CreateNamedPipeW(path, PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE_WAIT | PIPE_NOWAIT, 1, 65536, 65536, 0, None)
            if pipe == INVALID or pipe is None:
                print(f"cannot create {path}: error {ctypes.get_last_error()}", file=sys.stderr)
                return 1
            connected = False
            while time.monotonic() < end:
                if kernel32.ConnectNamedPipe(pipe, None) or ctypes.get_last_error() == ERROR_PIPE_CONNECTED:
                    connected = True
                    break
                time.sleep(0.1)
            if not connected:
                kernel32.CloseHandle(pipe)
                break
            # (back to blocking reads/writes now that the game is there)
            mode = wt.DWORD(PIPE_TYPE_BYTE_WAIT)
            kernel32.SetNamedPipeHandleState(pipe, ctypes.byref(mode), None, None)
            log(event="connected")
            while time.monotonic() < end:
                header = read_exact(pipe, 8, end)
                if not header:
                    break
                op, length = struct.unpack("<II", header)
                body = read_exact(pipe, length, end) if length else b""
                if body is None:
                    break
                try:
                    data = json.loads(body.decode()) if body else None
                except ValueError:
                    data = body.decode(errors="replace")
                log(op=op, data=data)
                if op == 0:
                    if args.reject:
                        write_frame(pipe, 2, {"code": 4000, "message": "Invalid Client ID"})
                    else:
                        write_frame(pipe, 1, {"cmd": "DISPATCH", "evt": "READY", "data": {"v": 1, "config": {}, "user": {"id": "0", "username": "test"}}, "nonce": None})
                elif op == 1 and isinstance(data, dict):
                    write_frame(pipe, 1, {"cmd": data.get("cmd"), "evt": None, "data": (data.get("args") or {}).get("activity"), "nonce": data.get("nonce")})
            log(event="disconnected")
            kernel32.DisconnectNamedPipe(pipe)
            kernel32.CloseHandle(pipe)
    return 0


if __name__ == "__main__":
    sys.exit(main())
