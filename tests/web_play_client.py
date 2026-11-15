#!/usr/bin/env python3
"""A browser, as little of one as Web Play needs: fetches the page over HTTPS,
joins over the WebSocket with one gamepad holding A, and waits for a keyframe
of the television.

Usage: web_play_client.py <port> <seconds>
Exit status 0 when the page, the join and a keyframe all arrived.
"""

import base64
import json
import os
import socket
import ssl
import struct
import sys
import time


def connect(port):
    context = ssl.create_default_context()
    context.check_hostname = False
    context.verify_mode = ssl.CERT_NONE
    raw = socket.create_connection(("127.0.0.1", port), timeout=5)
    return context.wrap_socket(raw, server_hostname="localhost")


def read_head(sock):
    data = b""
    while b"\r\n\r\n" not in data:
        chunk = sock.recv(4096)
        if not chunk:
            break
        data += chunk
    head, _, rest = data.partition(b"\r\n\r\n")
    return head.decode("latin-1"), rest


def send_frame(sock, opcode, payload):
    mask = os.urandom(4)
    header = bytes([0x80 | opcode])
    if len(payload) < 126:
        header += bytes([0x80 | len(payload)])
    else:
        header += bytes([0x80 | 126]) + struct.pack(">H", len(payload))
    masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
    sock.sendall(header + mask + masked)


class Reader:
    def __init__(self, sock, data):
        self.sock = sock
        self.data = data

    def need(self, count):
        while len(self.data) < count:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise EOFError("closed")
            self.data += chunk

    def frame(self):
        self.need(2)
        opcode = self.data[0] & 0x0F
        length = self.data[1] & 0x7F
        at = 2
        if length == 126:
            self.need(4)
            length = struct.unpack(">H", self.data[2:4])[0]
            at = 4
        elif length == 127:
            self.need(10)
            length = struct.unpack(">Q", self.data[2:10])[0]
            at = 10
        self.need(at + length)
        payload = self.data[at:at + length]
        self.data = self.data[at + length:]
        return opcode, payload


def main():
    port = int(sys.argv[1])
    seconds = float(sys.argv[2])

    page = connect(port)
    page.sendall(b"GET / HTTP/1.1\r\nHost: localhost\r\n\r\n")
    head, body = read_head(page)
    if " 200 " not in head.split("\r\n")[0]:
        print("the page wasn't served: " + head.split("\r\n")[0])
        return 1
    print("page: " + head.split("\r\n")[0])
    page.close()

    sock = connect(port)
    key = base64.b64encode(os.urandom(16)).decode()
    sock.sendall(("GET /ws HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                  "Sec-WebSocket-Key: " + key + "\r\nSec-WebSocket-Version: 13\r\n\r\n").encode())
    head, rest = read_head(sock)
    if " 101 " not in head.split("\r\n")[0]:
        print("no WebSocket: " + head.split("\r\n")[0])
        return 1
    reader = Reader(sock, rest)
    send_frame(sock, 1, json.dumps({"type": "hello", "name": "Test Browser"}).encode())
    send_frame(sock, 1, json.dumps({"type": "pads", "pads": [{"index": 0, "name": "Test Pad"}]}).encode())

    got_info = got_keyframe = got_status = False
    deadline = time.time() + seconds
    last_pads = 0
    sock.settimeout(0.2)
    while time.time() < deadline and not (got_info and got_keyframe and got_status):
        if time.time() - last_pads > 0.1:
            # A held, sticks centred.
            send_frame(sock, 2, bytes([16, 1, 0]) + struct.pack("<I", 1) + struct.pack("<6h", 0, 0, 0, 0, 0, 0))
            last_pads = time.time()
        try:
            opcode, payload = reader.frame()
        except socket.timeout:
            continue
        except (EOFError, ssl.SSLError, OSError) as error:
            print("lost the connection: " + str(error))
            return 1
        if opcode == 1:
            message = json.loads(payload)
            print("text: " + payload.decode()[:120])
            if message.get("type") == "info":
                got_info = True
            if message.get("type") == "status" and message.get("pads", {}).get("0"):
                got_status = True
        elif opcode == 2 and payload and payload[0] == 1:
            keyframe = bool(payload[2] & 1)
            width, height = struct.unpack("<HH", payload[3:7])
            if keyframe:
                # An SPS among the NAL units, so a browser can configure from it.
                if b"\x00\x00\x01\x67" in payload[7:] or b"\x00\x00\x01\x27" in payload[7:]:
                    got_keyframe = True
                    print("keyframe: %dx%d, %d bytes" % (width, height, len(payload) - 7))
    send_frame(sock, 8, b"")
    sock.close()
    if not got_info:
        print("never told what it was joining")
    if not got_keyframe:
        print("no keyframe of the television arrived")
    if not got_status:
        print("never told what its pad is")
    return 0 if got_info and got_keyframe and got_status else 1


if __name__ == "__main__":
    sys.exit(main())
