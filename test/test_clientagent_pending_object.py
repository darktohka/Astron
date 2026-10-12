#!/usr/bin/env python3
"""Regression test for a client-agent NULL-dereference/segfault in
Client::try_queue_pending().

Background
----------
While an interest operation ("iop") is pending, the client agent buffers
server messages for objects that have not been delivered yet.  It records that
buffering in m_pending_objects: a map of doid -> request_context of the iop
doing the buffering.

InterestOperation::finish() deletes the iop and erases it from
m_pending_interests, but it never removes the m_pending_objects entries that
point at it.  If such a stale entry survives, the next message for that object
reaches:

    Client::try_queue_pending()                        (Client.cpp)
        m_pending_interests.find(it->second)->second->queue_datagram(dg);

find() returns m_pending_interests.end(); dereferencing it (->second) is an
invalid pointer dereference and crashes the daemon.

Reproduction
------------
1. Open interest id=1 on (parent=1234, zone=4321)  -> iop R1 (ctx1) pending.
2. A STATESERVER_OBJECT_ENTER_LOCATION for object D in (1234, 4321) arrives;
   it is buffered inside R1 and m_pending_objects[D] = ctx1.
3. Remove interest id=1 (R1 stays pending; removing only clears m_interests).
4. Open interest id=2 on (1234, 4321)              -> iop R2 (ctx2) pending.
5. STATESERVER_OBJECT_GET_ZONES_COUNT_RESP(ctx1, 0) finishes R1.  R1
   redispatches the buffered ENTER_LOCATION; that now matches R2 and is
   re-queued into R2 *without* erasing m_pending_objects[D], which still points
   at the already-deleted R1.
6. STATESERVER_OBJECT_CHANGING_LOCATION for D -> try_queue_pending(D) ->
   m_pending_interests.find(ctx1) == end() -> crash.

On the vulnerable build the daemon dies at step 6.  On the fixed build it
survives and keeps serving; this script then verifies continued health.
"""
import os
import sys
import time
import socket

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from common.astron import Daemon, Datagram, DatagramIterator, ClientConnection, ChannelConnection
from common.astron import (USE_THREADING, CLIENT_HELLO, CLIENT_HELLO_RESP,
                           CLIENT_ADD_INTEREST, CLIENT_REMOVE_INTEREST,
                           CLIENT_OBJECT_SET_FIELD, CLIENTAGENT_SET_STATE,
                           CLIENT_STATE_ESTABLISHED,
                           STATESERVER_OBJECT_GET_ZONES_OBJECTS,
                           STATESERVER_OBJECT_GET_ZONES_COUNT_RESP,
                           STATESERVER_OBJECT_ENTER_LOCATION_WITH_REQUIRED,
                           STATESERVER_OBJECT_CHANGING_LOCATION,
                           STATESERVER_OBJECT_SET_FIELD)
from common.dcfile import test_dc, DC_HASH, request, DistributedTestObject1

MD_PORT = 57233
CA_PORT = 57238
VERSION = "Sword Art Online v5.1"

PARENT = 1234
ZONE = 4321
OBJECT = 8888

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
          interest_timeout: 5000
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


def recv_dg(conn, timeout=5.0):
    """Receive one datagram, retrying until `timeout` seconds elapse."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        dg = conn.recv_maybe()
        if dg is not None:
            return dg
        time.sleep(0.02)
    raise RuntimeError("timed out waiting for a datagram")


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

    resp = recv_dg(client)
    dgi = DatagramIterator(resp)
    assert dgi.read_uint16() == CLIENT_HELLO_RESP, "did not get CLIENT_HELLO_RESP"
    return client


def identify(client, server):
    """Ask the client agent to reveal the client's session channel."""
    dg = Datagram()
    dg.add_uint16(CLIENT_OBJECT_SET_FIELD)
    dg.add_doid(1234)
    dg.add_uint16(request)
    dg.add_string("What... am I?")
    client.send(dg)

    routed = recv_dg(server)
    dgi = DatagramIterator(routed)
    assert dgi.read_uint8() == 1
    assert dgi.read_channel() == 1234
    sender = dgi.read_channel()
    assert dgi.read_uint16() == STATESERVER_OBJECT_SET_FIELD
    return sender


def set_state(server, client_id, state):
    dg = Datagram.create([client_id], 1, CLIENTAGENT_SET_STATE)
    dg.add_uint16(state)
    server.send(dg)
    time.sleep(0.1)


def add_interest(client, server, client_id, interest_id, context):
    dg = Datagram()
    dg.add_uint16(CLIENT_ADD_INTEREST)
    dg.add_uint32(context)
    dg.add_uint16(interest_id)
    dg.add_doid(PARENT)
    dg.add_zone(ZONE)
    client.send(dg)

    routed = recv_dg(server)
    dgi = DatagramIterator(routed)
    ok, why = dgi.matches_header([PARENT], client_id, STATESERVER_OBJECT_GET_ZONES_OBJECTS)
    assert ok, "unexpected GET_ZONES_OBJECTS: %s" % why
    ss_context = dgi.read_uint32()
    assert dgi.read_doid() == PARENT
    assert dgi.read_uint16() == 1
    assert dgi.read_zone() == ZONE
    return ss_context


def remove_interest(client, interest_id, context):
    dg = Datagram()
    dg.add_uint16(CLIENT_REMOVE_INTEREST)
    dg.add_uint32(context)
    dg.add_uint16(interest_id)
    client.send(dg)


def main():
    Daemon.DAEMON_PATH = find_astrond()
    print("Using astrond: %s" % Daemon.DAEMON_PATH)

    daemon = Daemon(CONFIG)
    daemon.start()
    result = 1
    server = None
    client = None
    try:
        # Wire up a fake "server cluster" over the Message Director and the
        # uberdog channel the client will use to identify itself.
        server = ChannelConnection("127.0.0.1", MD_PORT, MDChannel=1234)

        client = connect_client()
        client_id = identify(client, server)
        print("Client session channel: %d" % client_id)
        set_state(server, client_id, CLIENT_STATE_ESTABLISHED)

        # 1. Open interest R1.
        ctx1 = add_interest(client, server, client_id, 1, 2000)
        print("R1 pending, request_context=%d" % ctx1)

        # 2. Object D enters the zone while R1 is pending; the CA buffers it
        #    inside R1 and records m_pending_objects[OBJECT] = ctx1.
        dg = Datagram.create([client_id], 1, STATESERVER_OBJECT_ENTER_LOCATION_WITH_REQUIRED)
        dg.add_doid(OBJECT)
        dg.add_doid(PARENT)
        dg.add_zone(ZONE)
        dg.add_uint16(DistributedTestObject1)
        dg.add_uint32(999999)
        server.send(dg)
        time.sleep(0.2)  # let the CA buffer it

        # 3. Remove interest id=1. R1 remains pending.
        remove_interest(client, 1, 2001)
        time.sleep(0.2)

        # 4. Open interest R2 on the same (parent, zone).
        ctx2 = add_interest(client, server, client_id, 2, 2002)
        print("R2 pending, request_context=%d" % ctx2)
        assert ctx1 != ctx2

        # 5. Finish R1. It redispatches the buffered ENTER_LOCATION, which is
        #    re-queued into R2, leaving m_pending_objects[OBJECT] stale.
        dg = Datagram.create([client_id], PARENT, STATESERVER_OBJECT_GET_ZONES_COUNT_RESP)
        dg.add_uint32(ctx1)
        dg.add_doid(0)  # no objects expected -> R1 is immediately complete
        server.send(dg)
        time.sleep(0.3)

        # 6. Trigger: a message for OBJECT hits try_queue_pending().
        dg = Datagram.create([client_id], 1, STATESERVER_OBJECT_CHANGING_LOCATION)
        dg.add_doid(OBJECT)
        dg.add_doid(PARENT)      # new parent
        dg.add_zone(ZONE + 1)    # new zone
        dg.add_doid(PARENT)      # old parent
        dg.add_zone(ZONE)        # old zone
        print("Sending CHANGING_LOCATION for a stale m_pending_objects entry...")
        server.send(dg)

        # Give the daemon a chance to crash / process the message.
        crashed = None
        for _ in range(60):
            time.sleep(0.05)
            if daemon.daemon is not None:
                crashed = daemon.daemon.poll()
                if crashed is not None:
                    break

        if crashed is not None:
            print("FAIL: client agent died (exit status %s) on a stale "
                  "m_pending_objects entry (try_queue_pending end() deref)." % crashed)
            return 1

        # The daemon must still be healthy and servicing the client.
        dg = Datagram()
        dg.add_uint16(CLIENT_OBJECT_SET_FIELD)
        dg.add_doid(1234)
        dg.add_uint16(request)
        dg.add_string("Still there?")
        client.send(dg)
        try:
            routed = recv_dg(server, timeout=5.0)
        except Exception as exc:
            print("FAIL: client agent stopped responding after the sequence: %s" % exc)
            return 1
        dgi = DatagramIterator(routed)
        dgi.read_uint8()
        dgi.read_channel()
        dgi.read_channel()
        assert dgi.read_uint16() == STATESERVER_OBJECT_SET_FIELD

        print("PASS: client agent survived the stale m_pending_objects entry "
              "and is still serving.")
        result = 0
    except Exception as exc:
        # If the daemon crashed, surface that rather than the follow-on
        # connection error.
        if daemon.daemon is not None and daemon.daemon.poll() is not None:
            print("FAIL: client agent died (exit status %s) - %s"
                  % (daemon.daemon.poll(), exc))
        else:
            print("ERROR: %r" % (exc,))
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
