#!/usr/bin/env python3
"""Regression test for cross-thread lifetime bugs in the client agent's interest
timeout machinery.

When the client agent receives a CLIENTAGENT_ADD_INTEREST from the cluster (as
opposed to from the client itself) the handling runs on the Message Director's
routing thread. Creating the interest operation there calls
Client::generate_timeout(), which cannot create the libuv Timeout object
directly (libuv is single threaded) and instead queues a task to be run on the
main thread.

Two objects can be destroyed during the window before that queued task runs:

  * the InterestOperation itself (a legitimate interest that finishes
    immediately -- for example one whose zone contains no objects), and
  * the Client, if it disconnects in the meantime.

The vulnerable code captured a raw `this` of each object in the queued task, so
when the task finally ran it dereferenced freed memory. The same is true of the
deferred Timeout::cancel() task, which the interest operation issues when it
finishes.

The fix gives each of these objects a reference-counted lifetime token that the
queued task and the object's destructor both synchronize on. This test drives
both races in a tight loop; under AddressSanitizer the vulnerable build aborts
with a heap-use-after-free, while the fixed build survives and keeps serving.
"""
import os
import sys
import time
import socket

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from common.astron import Daemon, Datagram, DatagramIterator, ClientConnection, ChannelConnection
from common.astron import (USE_THREADING, CLIENT_HELLO, CLIENT_HELLO_RESP,
                           CLIENT_HEARTBEAT,
                           CLIENT_OBJECT_SET_FIELD, CLIENTAGENT_SET_STATE,
                           CLIENT_STATE_ESTABLISHED, CLIENTAGENT_ADD_INTEREST,
                           STATESERVER_OBJECT_GET_ZONES_OBJECTS,
                           STATESERVER_OBJECT_GET_ZONES_COUNT_RESP,
                           STATESERVER_OBJECT_SET_FIELD)
from common.dcfile import test_dc, DC_HASH, request

MD_PORT = 57333
CA_PORT = 57338
VERSION = "Sword Art Online v5.1"

PARENT = 1234
ZONE = 4321
# Number of "create an interest and finish it immediately" cycles.
IOP_CYCLES = 400
# Number of "create an interest then immediately drop the client" cycles.
CLIENT_CYCLES = 100

CONFIG = """\
messagedirector:
    bind: 127.0.0.1:%d
    threaded: %s

general:
    dc_files:
        - %r

uberdogs:
    - id: 1234
      class: UberDog1
      anonymous: true

roles:
    - type: clientagent
      bind: 127.0.0.1:%d
      version: %r
      channels:
          min: 3100
          max: 3999
      client:
          add_interest: enabled
          write_buffer_size: 0
          write_timeout_ms: 0
      tuning:
          interest_timeout: 50
""" % (MD_PORT, USE_THREADING, test_dc, CA_PORT, VERSION)


def find_astrond():
    if "ASTROND" in os.environ:
        return os.environ["ASTROND"]
    repo = os.path.dirname(HERE)
    for candidate in ("./astrond", os.path.join(repo, "build", "astrond"),
                      os.path.join(repo, "astrond")):
        if os.path.exists(candidate):
            return os.path.abspath(candidate)
    return "./astrond"


def connect_client():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(5.0)
    s.connect(("127.0.0.1", CA_PORT))
    client = ClientConnection(s)

    dg = Datagram()
    dg.add_uint16(CLIENT_HELLO)
    dg.add_uint32(DC_HASH)
    dg.add_string(VERSION)
    client.send(dg)

    deadline = time.time() + 5.0
    while time.time() < deadline:
        resp = client.recv_maybe()
        if resp is not None:
            break
        time.sleep(0.01)
    else:
        raise RuntimeError("no CLIENT_HELLO_RESP")
    dgi = DatagramIterator(resp)
    assert dgi.read_uint16() == CLIENT_HELLO_RESP
    return client


def identify(client, server):
    """Ask the client agent to reveal the client's session channel."""
    dg = Datagram()
    dg.add_uint16(CLIENT_OBJECT_SET_FIELD)
    dg.add_doid(1234)
    dg.add_uint16(request)
    dg.add_string("What... am I?")
    client.send(dg)
    dg, dgi = wait_for_msgtype(server, STATESERVER_OBJECT_SET_FIELD)
    dgi.seek(0)
    dgi.read_uint8()
    assert dgi.read_channel() == 1234
    return dgi.read_channel()


def set_state(server, client_id, state):
    dg = Datagram.create([client_id], 1, CLIENTAGENT_SET_STATE)
    dg.add_uint16(state)
    server.send(dg)
    time.sleep(0.05)


def wait_for_msgtype(conn, msgtype, timeout=5.0):
    """Read server->cluster datagrams until `msgtype` is seen.

    The client agent also sends CLIENTAGENT_DONE_INTEREST_RESP back to the
    caller, so unrelated datagrams must be tolerated.
    """
    deadline = time.time() + timeout
    while time.time() < deadline:
        dg = conn.recv_maybe()
        if dg is None:
            time.sleep(0.005)
            continue
        dgi = DatagramIterator(dg)
        n = dgi.read_uint8()
        for _ in range(n):
            dgi.read_channel()
        dgi.read_channel()  # sender
        if dgi.read_uint16() == msgtype:
            return dg, dgi
    raise RuntimeError("timed out waiting for msgtype %d" % msgtype)


def server_add_interest(server, client_id, interest_id, zone):
    """Start a server-initiated interest and return its request_context."""
    dg = Datagram.create([client_id], 1, CLIENTAGENT_ADD_INTEREST)
    dg.add_uint16(interest_id)
    dg.add_doid(PARENT)
    dg.add_zone(zone)
    server.send(dg)
    _, dgi = wait_for_msgtype(server, STATESERVER_OBJECT_GET_ZONES_OBJECTS)
    return dgi.read_uint32()


def send_add_interest(server, client_id, interest_id, zone):
    dg = Datagram.create([client_id], 1, CLIENTAGENT_ADD_INTEREST)
    dg.add_uint16(interest_id)
    dg.add_doid(PARENT)
    dg.add_zone(zone)
    server.send(dg)


def send_finish_interest(server, client_id, request_context):
    dg = Datagram.create([client_id], 1, STATESERVER_OBJECT_GET_ZONES_COUNT_RESP)
    dg.add_uint32(request_context)
    dg.add_doid(0)  # no objects -> the operation finishes right away
    server.send(dg)


def drain(conn):
    while conn.recv_maybe() is not None:
        pass


def finish_interest(server, client_id, request_context):
    send_finish_interest(server, client_id, request_context)


def main():
    Daemon.DAEMON_PATH = find_astrond()
    print("Using astrond: %s" % Daemon.DAEMON_PATH)

    daemon = Daemon(CONFIG)
    daemon.start()
    server = None
    client = None
    result = 1
    try:
        server = ChannelConnection("127.0.0.1", MD_PORT, MDChannel=PARENT)

        client = connect_client()
        client_id = identify(client, server)
        set_state(server, client_id, CLIENT_STATE_ESTABLISHED)

        # Phase A: interest operations that are created and destroyed on the
        # routing thread before the main thread gets to create their Timeout.
        #
        # We learn the initial request_context from a single round-tripped
        # operation, then fire the add and the completion back to back (the
        # request_context advances by exactly two per operation) so the routing
        # thread finishes the operation before the main thread can run the
        # deferred timeout-creation task.
        print("Phase A: %d create-and-finish interest cycles..." % IOP_CYCLES)
        base_ctx = server_add_interest(server, client_id, 0, ZONE)
        finish_interest(server, client_id, base_ctx)
        for i in range(1, IOP_CYCLES):
            send_add_interest(server, client_id, i & 0xffff, ZONE + i)
            send_finish_interest(server, client_id, base_ctx + 2 * i)
            if i % 16 == 0:
                drain(server)
            if daemon.daemon.poll() is not None:
                break

        # Phase B: interest operations whose Client is destroyed right after
        # the operation is created on the routing thread.
        print("Phase B: %d create-then-disconnect cycles..." % CLIENT_CYCLES)
        for i in range(CLIENT_CYCLES):
            if daemon.daemon.poll() is not None:
                break
            send_add_interest(server, client_id, i & 0xffff, 100000 + i)
            client.close()
            # Nudge the message director so the routing thread reaps the
            # disconnected client.
            server.send(Datagram.create([PARENT], 1, CLIENT_HEARTBEAT))
            drain(server)
            client = connect_client()
            client_id = identify(client, server)
            set_state(server, client_id, CLIENT_STATE_ESTABLISHED)

        # Give the daemon a moment to process any leftover deferred tasks.
        for _ in range(40):
            time.sleep(0.05)
            if daemon.daemon.poll() is not None:
                break

        if daemon.daemon.poll() is not None:
            print("FAIL: client agent died (exit status %s) during interest "
                  "timeout lifetime cycling." % daemon.daemon.poll())
            return 1

        # The daemon must still be alive and servicing the client.
        dg = Datagram()
        dg.add_uint16(CLIENT_OBJECT_SET_FIELD)
        dg.add_doid(1234)
        dg.add_uint16(request)
        dg.add_string("Still there?")
        client.send(dg)
        try:
            wait_for_msgtype(server, STATESERVER_OBJECT_SET_FIELD)
        except Exception as exc:
            print("FAIL: client agent stopped responding: %s" % exc)
            return 1

        print("PASS: client agent survived interest timeout lifetime cycling.")
        result = 0
    except Exception as exc:
        if daemon.daemon is not None and daemon.daemon.poll() is not None:
            print("FAIL: client agent died (exit status %s) - %s"
                  % (daemon.daemon.poll(), exc))
        else:
            import traceback
            traceback.print_exc()
        result = 1
    finally:
        for c in (client, server):
            try:
                if c is not None:
                    c.close()
            except Exception:
                pass
        daemon.stop()
    return result


if __name__ == "__main__":
    sys.exit(main())
