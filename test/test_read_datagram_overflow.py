#!/usr/bin/env python3
"""Regression test for the read_datagram() out-of-bounds read (Message Director).

Any TCP client that can reach the Message Director port is accepted as a
"network participant" and may send control messages. CONTROL_ADD_POST_REMOVE
(9010) is parsed as:

    [uint8 num_channels=1][channel CONTROL_MESSAGE=1]
    [uint16 msg_type=9010][channel sender][dgsize_t length][blob...]

DatagramIterator::read_datagram() used to read that length and hand it straight
to Datagram::create(), which memcpy()'s `length` bytes out of the source
datagram with no bounds check. A truncated message that claims a length of
DGSIZE_MAX (65535) while supplying zero blob bytes therefore made the daemon
read ~64 KiB past the end of a ~21-byte heap buffer (heap-buffer-overflow READ,
confirmed under AddressSanitizer).

With the fix, read_datagram() bounds-checks the length and throws
DatagramIteratorEOF, which MDNetworkParticipant turns into a clean participant
teardown. This test sends that exact message and asserts that the daemon
(a) survives and (b) actually terminates the offending participant. On the
vulnerable build the over-read happens silently and the participant is left
connected, so the test fails deterministically.
"""
import os
import sys
import time
import socket

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from common.astron import Daemon, Datagram, ChannelConnection, DGSIZE_MAX
from common.astron import CONTROL_ADD_POST_REMOVE

MD_PORT = 57229

CONFIG = """\
messagedirector:
    bind: 127.0.0.1:%d
""" % MD_PORT


def find_astrond():
    if "ASTROND" in os.environ:
        return os.environ["ASTROND"]
    repo = os.path.dirname(HERE)
    for candidate in ("./astrond", os.path.join(repo, "build", "astrond"),
                      os.path.join(repo, "astrond")):
        if os.path.exists(candidate):
            return os.path.abspath(candidate)
    return "./astrond"


def build_truncated_post_remove():
    # A well-formed control header with a post-remove sender, followed by a
    # length tag that claims a full-size datagram but no actual blob bytes.
    dg = Datagram.create_control()
    dg.add_uint16(CONTROL_ADD_POST_REMOVE)
    dg.add_channel(0xDEADBEEF)  # arbitrary sender channel
    dg.add_size(DGSIZE_MAX)     # lie: claim DGSIZE_MAX bytes follow
    return dg


def participant_was_dropped(attacker, driver, timeout=5.0):
    """Return True if the daemon closed the attacker connection.

    A terminated participant is only reaped when the Message Director next
    routes a datagram (process_terminates() runs at the end of route_datagram),
    so send well-formed traffic on a second connection to drive that path.
    """
    sock = attacker.s
    # Give the daemon time to process the malformed control message first.
    time.sleep(0.3)

    for _ in range(10):
        driver.send(Datagram.create([0x1111], 0x2222, 0x3333))
        sock.settimeout(0.5)
        try:
            data = sock.recv(16)
        except socket.timeout:
            continue                  # still connected; reap again
        except (ConnectionResetError, BrokenPipeError, OSError):
            return True               # torn down
        return data == b''            # b'' -> clean FIN
    return False


def main():
    Daemon.DAEMON_PATH = find_astrond()
    print("Using astrond: %s" % Daemon.DAEMON_PATH)

    daemon = Daemon(CONFIG)
    daemon.start()
    result = 1
    try:
        attacker = ChannelConnection("127.0.0.1", MD_PORT)
        driver = ChannelConnection("127.0.0.1", MD_PORT, MDChannel=0x1111)

        dg = build_truncated_post_remove()
        print("Sending truncated CONTROL_ADD_POST_REMOVE (%d bytes, claims a "
              "%d-byte embedded datagram)..." % (len(dg.get_data()), DGSIZE_MAX))
        attacker.send(dg)

        dropped = participant_was_dropped(attacker, driver)
        time.sleep(0.3)

        for c in (attacker, driver):
            try:
                c.close()
            except Exception:
                pass

        proc = daemon.daemon
        if proc is not None:
            rc = proc.poll()
            if rc is not None:
                print("FAIL: message director died (exit status %s) handling a "
                      "truncated CONTROL_ADD_POST_REMOVE." % rc)
                return 1

        if not dropped:
            print("FAIL: message director accepted a truncated CONTROL_ADD_POST_REMOVE; "
                  "read_datagram() did not reject the oversized embedded length.")
            return 1

        # The daemon must still be healthy: a fresh participant must be accepted.
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(5.0)
        try:
            s.connect(("127.0.0.1", MD_PORT))
        except OSError as exc:
            print("FAIL: message director is no longer accepting connections: %s" % exc)
            return 1
        finally:
            try:
                s.close()
            except Exception:
                pass

        print("PASS: message director rejected the truncated post-remove and is still serving.")
        result = 0
    finally:
        daemon.stop()
    return result


if __name__ == "__main__":
    sys.exit(main())
