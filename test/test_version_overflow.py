#!/usr/bin/env python3
"""Recreation / regression test for the oversized CLIENT_HELLO version crash.

The Astron client agent reads the client's version string out of CLIENT_HELLO and,
when it does not match the server's version, echoes both back to the client in a
"Client version mismatch" error message. That message is longer than the string
the client sent, so a version string close to the 16-bit datagram size limit
(65535 bytes) produces an error string larger than DGSIZE_MAX.

The size checks in Datagram::add_string()/add_data()/add_blob() took a dgsize_t
parameter, so the caller's size_t length was truncated to 16 bits before the
overflow check ran. The length tag and the capacity check therefore used the
truncated length, but the following memcpy() copied the full length - writing
tens of kilobytes past the end of the datagram's heap buffer and crashing the
client agent.

This test connects to a client agent, sends the largest version string the
protocol allows, and then verifies the daemon is still alive and able to serve
a subsequent, well-behaved client.

Run with:  python3 test/test_version_overflow.py
or via ctest:  ctest -R clientagent_version_overflow
"""
import os
import sys
import time
import socket

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from common.astron import Daemon, Datagram, DatagramIterator, ClientConnection, DGSIZE_MAX
from common.astron import CLIENT_HELLO, CLIENT_HELLO_RESP
from common.dcfile import test_dc, DC_HASH

SERVER_VERSION = "Sword Art Online v5.1"
CA_PORT = 57228
MD_PORT = 57229

CONFIG = """\
messagedirector:
    bind: 127.0.0.1:%d

general:
    dc_files:
        - %r

roles:
    - type: clientagent
      bind: 127.0.0.1:%d
      version: "%s"
      channels:
          min: 3100
          max: 3999
""" % (MD_PORT, test_dc, CA_PORT, SERVER_VERSION)


def find_astrond():
    if "ASTROND" in os.environ:
        return os.environ["ASTROND"]
    repo = os.path.dirname(HERE)
    for candidate in ("./astrond", os.path.join(repo, "build", "astrond"), os.path.join(repo, "astrond")):
        if os.path.exists(candidate):
            return os.path.abspath(candidate)
    return "./astrond"


def build_oversized_hello():
    # A client datagram must itself fit inside a 16-bit length tag, so the
    # version string is capped at DGSIZE_MAX minus the other fields:
    # [uint16 msg_type][uint32 dc_hash][uint16 version length][version bytes]
    overhead = 2 + 4 + 2
    version = "A" * (DGSIZE_MAX - overhead)
    dg = Datagram()
    dg.add_uint16(CLIENT_HELLO)
    dg.add_uint32(DC_HASH)
    dg.add_string(version)
    assert len(dg.get_data()) == DGSIZE_MAX, len(dg.get_data())
    return dg, version


def read_msgtype(conn):
    resp = conn.recv_maybe()
    if resp is None:
        return None
    return DatagramIterator(resp).read_uint16()


def main():
    Daemon.DAEMON_PATH = find_astrond()
    print("Using astrond: %s" % Daemon.DAEMON_PATH)

    daemon = Daemon(CONFIG)
    daemon.start()
    result = 1
    try:
        # 1. Send the oversized version string.
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.connect(("127.0.0.1", CA_PORT))
        client = ClientConnection(s)
        dg, version = build_oversized_hello()
        print("Sending CLIENT_HELLO with a %d-byte version string..." % len(version))
        client.send(dg)

        # Give the daemon a moment to (mis)handle it.
        time.sleep(1.0)

        proc = daemon.daemon
        if proc is not None:
            rc = proc.poll()
            if rc is not None:
                print("FAIL: client agent died (exit status %s) handling an oversized "
                      "version string." % rc)
                return 1
        try:
            client.close()
        except Exception:
            pass

        # 2. The daemon must still be healthy: a well-behaved client should be
        #    able to complete the handshake.
        s2 = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s2.settimeout(5.0)
        s2.connect(("127.0.0.1", CA_PORT))
        good = ClientConnection(s2)
        hello = Datagram()
        hello.add_uint16(CLIENT_HELLO)
        hello.add_uint32(DC_HASH)
        hello.add_string(SERVER_VERSION)
        good.send(hello)
        msgtype = read_msgtype(good)
        try:
            good.close()
        except Exception:
            pass

        if msgtype != CLIENT_HELLO_RESP:
            print("FAIL: client agent is not serving new clients (got msgtype %r, "
                  "expected CLIENT_HELLO_RESP=%d)." % (msgtype, CLIENT_HELLO_RESP))
            return 1

        print("PASS: client agent survived the oversized version string and is still serving.")
        result = 0
    finally:
        daemon.stop()
    return result


if __name__ == "__main__":
    sys.exit(main())
