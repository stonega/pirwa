#!/usr/bin/env python3
"""Real gateway + synthetic QEMU Ethernet; loopback fixtures, no daemon or VM."""
import contextlib
import ipaddress
import os
from pathlib import Path
import select
import socket
import struct
import subprocess
import sys
import threading
import time
import unittest

HELPER = str(Path(sys.argv.pop(1)).resolve())
GUEST_MAC = bytes.fromhex('525400123456')
GATEWAY_MAC = bytes.fromhex('52550a000202')
GUEST_IP = '10.0.2.15'
PAYLOAD = b'opaque TCP payload\x00\xff\x16\x03\x03'
DNS_QUERY = bytes.fromhex('123401000001000000000000') + b'\x07example\x04test\0\0\x01\0\x01'
DNS_ANSWER = (bytes.fromhex('123481800001000100000000') + DNS_QUERY[12:] +
              bytes.fromhex('c00c000100010000003c0004c0000250'))


def exact(stream, size):
    result = bytearray()
    while len(result) < size:
        block = stream.recv(size - len(result))
        if not block:
            raise EOFError('short socket read')
        result.extend(block)
    return bytes(result)


def checksum(data):
    if len(data) % 2:
        data += b'\0'
    value = sum(struct.unpack(f'!{len(data) // 2}H', data))
    while value >> 16:
        value = (value & 65535) + (value >> 16)
    return (~value) & 65535


def ipv4(payload, protocol, source=GUEST_IP, destination='198.51.100.7', broadcast=False):
    source, destination = socket.inet_aton(source), socket.inet_aton(destination)
    header = struct.pack('!BBHHHBBH4s4s', 0x45, 0, 20 + len(payload), 1, 0, 64,
                         protocol, 0, source, destination)
    header = header[:10] + struct.pack('!H', checksum(header)) + header[12:]
    return (b'\xff' * 6 if broadcast else GATEWAY_MAC) + GUEST_MAC + b'\x08\0' + header + payload


def udp(payload, sport, dport, destination='10.0.2.3', source=GUEST_IP, broadcast=False):
    return ipv4(struct.pack('!HHHH', sport, dport, 8 + len(payload), 0) + payload,
                17, source, destination, broadcast)


def tcp(payload=b'', sport=40000, dport=443, seq=1000, ack=0, flags=2,
        destination='198.51.100.7'):
    header = struct.pack('!HHIIBBHHH', sport, dport, seq, ack, 0x50, flags, 65535, 0, 0)
    pseudo = socket.inet_aton(GUEST_IP) + socket.inet_aton(destination) + struct.pack('!BBH', 0, 6,
                                                                                   20 + len(payload))
    header = header[:16] + struct.pack('!H', checksum(pseudo + header + payload)) + header[18:]
    return ipv4(header + payload, 6, destination=destination)


def dhcp(message):
    bootp = bytearray(240)
    struct.pack_into('!BBBBIHH', bootp, 0, 1, 1, 6, 0, 0x12345678, 0, 0x8000)
    bootp[28:34] = GUEST_MAC
    bootp[236:240] = bytes.fromhex('63825363')
    options = bytes([53, 1, message, 55, 3, 1, 3, 6])
    if message == 3:
        options += bytes([50, 4, 10, 0, 2, 15, 54, 4, 10, 0, 2, 2])
    return udp(bootp + options + b'\xff', 68, 67, '255.255.255.255', '0.0.0.0', True)


class Proxy:
    """A SOCKS or HTTP endpoint. It never opens an Internet connection."""
    def __init__(self, kind='socks5', reject=False, hold=False):
        self.kind, self.reject, self.hold = kind, reject, hold
        self.listener = socket.socket()
        self.listener.bind(('127.0.0.1', 0))
        self.listener.listen(64)
        self.listener.settimeout(.1)
        self.port = self.listener.getsockname()[1]
        self.requests, self.payloads, self.errors = [], [], []
        self.stop = threading.Event()
        self.threads, self.connections = [], []
        self.thread = threading.Thread(target=self.accept, daemon=True)
        self.thread.start()

    def accept(self):
        while not self.stop.is_set():
            try:
                conn, _ = self.listener.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            conn.settimeout(3)
            self.connections.append(conn)
            thread = threading.Thread(target=self.handle, args=(conn,), daemon=True)
            self.threads.append(thread)
            thread.start()

    def handle(self, conn):
        try:
            with conn:
                if self.kind == 'socks5':
                    version, methods = exact(conn, 2)
                    assert version == 5
                    assert 0 in exact(conn, methods)
                    conn.sendall(b'\x05\0')
                    version, command, reserved, kind = exact(conn, 4)
                    assert (version, command, reserved) == (5, 1, 0)
                    if kind == 1:
                        host = socket.inet_ntoa(exact(conn, 4))
                    elif kind == 3:
                        host = exact(conn, exact(conn, 1)[0]).decode()
                    else:
                        host = str(ipaddress.IPv6Address(exact(conn, 16)))
                    port = struct.unpack('!H', exact(conn, 2))[0]
                    self.requests.append((host, port))
                    if self.hold:
                        self.stop.wait(10)
                        return
                    conn.sendall(bytes([5, 5 if self.reject else 0, 0, 1]) + b'\0' * 6)
                else:
                    header = bytearray()
                    while not header.endswith(b'\r\n\r\n'):
                        header += exact(conn, 1)
                        assert len(header) < 8192
                    method, address, version = bytes(header).split(b'\r\n')[0].split()
                    assert method == b'CONNECT' and version.startswith(b'HTTP/')
                    host, port = address.decode().rsplit(':', 1)
                    port = int(port)
                    self.requests.append((host, port))
                    conn.sendall(b'HTTP/1.0 502 Bad Gateway\r\n\r\n' if self.reject else
                                 b'HTTP/1.0 200 Connection established\r\n\r\n')
                if self.reject:
                    return
                if port == 53:
                    length = struct.unpack('!H', exact(conn, 2))[0]
                    query = exact(conn, length)
                    assert query == DNS_QUERY
                    conn.sendall(struct.pack('!H', len(DNS_ANSWER)) + DNS_ANSWER)
                    conn.shutdown(socket.SHUT_WR)
                else:
                    value = exact(conn, len(PAYLOAD))
                    self.payloads.append(value)
                    conn.sendall(value)
                    conn.shutdown(socket.SHUT_WR)
                    # Keep the read half open until the guest FIN, just as a server can.
                    with contextlib.suppress(socket.timeout, OSError):
                        while conn.recv(4096):
                            pass
        except (EOFError, ConnectionError, socket.timeout):
            pass
        except Exception as exc:
            self.errors.append(repr(exc))

    def close(self):
        self.stop.set()
        self.listener.close()
        self.thread.join(2)
        for conn in self.connections:
            with contextlib.suppress(OSError):
                conn.shutdown(socket.SHUT_RDWR)
        for thread in self.threads:
            thread.join(2)


class Gateway:
    def __init__(self, proxy):
        self.expected_status = 0
        listener = socket.socket()
        listener.bind(('127.0.0.1', 0))
        listener.listen(1)
        parent, child = socket.socketpair()
        self.process = subprocess.Popen([HELPER, '--listen-fd', str(listener.fileno()),
                                         '--control-fd', str(child.fileno())],
                                        pass_fds=(listener.fileno(), child.fileno()),
                                        stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                        stderr=subprocess.PIPE)
        child.close()
        config = f"('{proxy.kind}://127.0.0.1:{proxy.port}', '', '1.1.1.1', @as [])".encode()
        parent.settimeout(3)
        try:
            parent.sendall(struct.pack('!I', len(config)) + config)
            assert exact(parent, 1) == b'1'
            self.socket = socket.create_connection(listener.getsockname(), timeout=3)
        except Exception:
            self.process.terminate()
            self.process.wait(3)
            raise AssertionError(self.process.stderr.read().decode())
        finally:
            listener.close()
            parent.close()

    def send(self, frame):
        self.socket.sendall(struct.pack('!I', len(frame)) + frame)

    def receive(self, predicate, timeout=3):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.socket.settimeout(max(.01, deadline - time.monotonic()))
            frame = exact(self.socket, struct.unpack('!I', exact(self.socket, 4))[0])
            if len(frame) >= 34 and frame[12:14] == b'\x08\0':
                assert checksum(frame[14:14 + (frame[14] & 15) * 4]) == 0
            if predicate(frame):
                return frame
        raise AssertionError('expected Ethernet response missing')

    def arp(self):
        arp = struct.pack('!HHBBH6s4s6s4s', 1, 0x0800, 6, 4, 1, GUEST_MAC,
                          socket.inet_aton(GUEST_IP), b'\0' * 6, socket.inet_aton('10.0.2.2'))
        self.send(b'\xff' * 6 + GUEST_MAC + b'\x08\x06' + arp)
        frame = self.receive(lambda f: f[12:14] == b'\x08\x06')
        assert frame[20:22] == b'\0\x02'

    def open_tcp(self, destination='198.51.100.7', port=443, sport=40000):
        self.send(tcp(sport=sport, dport=port, destination=destination))

        syn = self.receive(lambda f: len(f) >= 54 and f[23] == 6 and
                           struct.unpack('!HH', f[34:38]) == (port, sport) and
                           f[47] & 0x12 == 0x12)
        server = struct.unpack('!I', syn[38:42])[0] + 1
        self.send(tcp(sport=sport, dport=port, destination=destination, seq=1001, ack=server,
                      flags=0x10))
        return server

    def tcp(self, destination='198.51.100.7', port=443, payload=PAYLOAD, sport=40000):
        server = self.open_tcp(destination, port, sport)

        def match(frame):
            if len(frame) < 54 or frame[23] != 6:
                return False
            offset = 14 + (frame[14] & 15) * 4
            return struct.unpack('!HH', frame[offset:offset + 4]) == (port, sport)

        self.send(tcp(payload, sport=sport, dport=port, destination=destination, seq=1001,
                      ack=server, flags=0x18))
        received = bytearray()
        fin = False
        deadline = time.monotonic() + 4
        while len(received) < len(payload) or not fin:
            frame = self.receive(match, max(.01, deadline - time.monotonic()))
            offset = 14 + (frame[14] & 15) * 4
            length = struct.unpack('!H', frame[16:18])[0]
            segment = frame[offset:14 + length]
            pseudo = frame[26:34] + struct.pack('!BBH', 0, 6, len(segment))
            assert checksum(pseudo + segment) == 0
            data = segment[(segment[12] >> 4) * 4:]
            seq = struct.unpack('!I', segment[4:8])[0]
            if seq == server:
                received += data
                server += len(data)
                if segment[13] & 1:
                    server += 1
                    fin = True
            self.send(tcp(sport=sport, dport=port, destination=destination,
                          seq=1001 + len(payload), ack=server, flags=0x10))
        self.send(tcp(sport=sport, dport=port, destination=destination,
                      seq=1001 + len(payload), ack=server, flags=0x11))
        return bytes(received)

    def close(self):
        self.socket.close()
        try:
            self.process.wait(3)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait()
            raise AssertionError('gateway did not exit after the guest disconnected')
        stderr = self.process.stderr.read().decode()
        self.process.stderr.close()
        assert self.process.returncode == self.expected_status, (
            f'gateway exit {self.process.returncode}: {stderr}')
        assert not stderr, stderr


class Tests(unittest.TestCase):
    def setUp(self):
        self.resources = []

    def tearDown(self):
        for resource in reversed(self.resources):
            resource.close()

    def gateway(self, kind='socks5', **kwargs):
        proxy = Proxy(kind, **kwargs)
        self.resources.append(proxy)
        gateway = Gateway(proxy)
        self.resources.append(gateway)
        gateway.arp()
        return proxy, gateway

    def test_dhcp_tcp_dns(self):
        proxy, gateway = self.gateway()
        for request, expected in ((1, 2), (3, 5)):
            gateway.send(dhcp(request))
            frame = gateway.receive(lambda f: len(f) >= 300 and f[23] == 17 and
                                    struct.unpack('!H', f[34:36])[0] == 67)
            bootp = frame[42:]
            self.assertEqual(bootp[16:20], socket.inet_aton(GUEST_IP))
            options, offset = {}, 240
            while offset < len(bootp) and bootp[offset] != 255:
                code = bootp[offset]
                offset += 1
                if not code:
                    continue
                length = bootp[offset]
                options[code] = bootp[offset + 1:offset + 1 + length]
                offset += length + 1
            self.assertEqual(options[53], bytes([expected]))
            self.assertEqual(options[1], b'\xff\xff\xff\0')
            self.assertEqual(options[3], socket.inet_aton('10.0.2.2'))
            self.assertEqual(options[6], socket.inet_aton('10.0.2.3'))
        self.assertEqual(gateway.tcp(), PAYLOAD)
        gateway.send(udp(DNS_QUERY, 44444, 53))
        answer = gateway.receive(lambda f: len(f) > 42 and f[23] == 17 and f[36:38] ==
                                 struct.pack('!H', 44444))
        self.assertEqual(answer[42:], DNS_ANSWER)
        self.assertEqual(gateway.tcp(destination='10.0.2.3', port=53, sport=40001,
                                     payload=struct.pack('!H', len(DNS_QUERY)) + DNS_QUERY),
                         struct.pack('!H', len(DNS_ANSWER)) + DNS_ANSWER)
        self.assertIn(('198.51.100.7', 443), proxy.requests)
        self.assertIn(('1.1.1.1', 53), proxy.requests)
        self.assertEqual(proxy.payloads, [PAYLOAD])
        self.assertFalse(proxy.errors)

    def test_http_connect(self):
        proxy, gateway = self.gateway('http')
        self.assertEqual(gateway.tcp(), PAYLOAD)
        self.assertEqual(proxy.requests, [('198.51.100.7', 443)])
        self.assertFalse(proxy.errors)

    def test_proxy_failure_never_connects_directly(self):
        proxy, gateway = self.gateway(reject=True)
        with socket.socket() as target:
            target.bind(('127.0.0.1', 0))
            target.listen(1)
            port = target.getsockname()[1]
            gateway.open_tcp(destination='127.0.0.1', port=port)
            deadline = time.monotonic() + 2
            while not proxy.requests and time.monotonic() < deadline:
                time.sleep(.01)
            self.assertEqual(proxy.requests, [('127.0.0.1', port)])
            self.assertFalse(select.select([target], [], [], .1)[0])

    def test_udp_and_ipv6_never_go_directly(self):
        proxy, gateway = self.gateway()
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as target:
            target.bind(('127.0.0.1', 0))
            gateway.send(udp(b'must not escape', 44444, target.getsockname()[1], '127.0.0.1'))
            gateway.send(GATEWAY_MAC + GUEST_MAC + b'\x86\xdd' + b'\x60' + b'\0' * 47)
            self.assertFalse(select.select([target], [], [], .2)[0])
            self.assertFalse(proxy.requests)

    def test_shutdown_with_proxy_handshake_pending(self):
        proxy, gateway = self.gateway(hold=True)
        gateway.open_tcp()
        deadline = time.monotonic() + 2
        while not proxy.requests and time.monotonic() < deadline:
            time.sleep(.01)
        self.assertTrue(proxy.requests)
        started = time.monotonic()
        gateway.process.terminate()
        gateway.process.wait(3)
        self.assertLess(time.monotonic() - started, 3)

    def test_many_pending_connections_keep_gateway_responsive(self):
        proxy, gateway = self.gateway(hold=True)
        for i in range(32):
            gateway.open_tcp(sport=40000 + i)
        gateway.arp()
        self.assertLessEqual(len(proxy.requests), 8)
        gateway.process.terminate()
        gateway.process.wait(3)

    def test_malformed_framing_exits(self):
        _, gateway = self.gateway()
        gateway.expected_status = 1
        gateway.socket.sendall(struct.pack('!I', 65537))
        gateway.process.wait(3)


if __name__ == '__main__':
    unittest.main()
