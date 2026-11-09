#!/usr/bin/env python3
"""Minimal Modbus TCP server used by smoke_client.cpp.

It implements only the function codes exercised by the smoke test:
0x03, 0x04, 0x06 and 0x10. No third-party package is required.
"""
import argparse
import socket
import socketserver
import struct
import sys
import threading
import time

HOLDING = [100 + i for i in range(32)]
INPUT = [200 + i for i in range(32)]
LOCK = threading.Lock()
REQUESTS = 0
DROP_AFTER = 0
SLOW_MS = 0
BLACKHOLE = False


def exception_pdu(function, code):
    return struct.pack(">BB", function | 0x80, code)


def handle_pdu(unit, pdu):
    function = pdu[0]
    if function in (0x03, 0x04):
        if len(pdu) != 5:
            return exception_pdu(function, 0x03)
        start, count = struct.unpack(">HH", pdu[1:5])
        if count < 1 or count > 125:
            return exception_pdu(function, 0x03)
        table = HOLDING if function == 0x03 else INPUT
        if start + count > len(table):
            return exception_pdu(function, 0x02)
        with LOCK:
            values = table[start:start + count]
        return struct.pack(">BB", function, count * 2) + b"".join(
            struct.pack(">H", value) for value in values
        )

    if function == 0x06:
        if len(pdu) != 5:
            return exception_pdu(function, 0x03)
        address, value = struct.unpack(">HH", pdu[1:5])
        if address >= len(HOLDING):
            return exception_pdu(function, 0x02)
        with LOCK:
            HOLDING[address] = value
        return pdu

    if function == 0x10:
        if len(pdu) < 6:
            return exception_pdu(function, 0x03)
        address, count, byte_count = struct.unpack(">HHB", pdu[1:6])
        if count < 1 or count > 123 or byte_count != count * 2:
            return exception_pdu(function, 0x03)
        if address + count > len(HOLDING):
            return exception_pdu(function, 0x02)
        values = struct.unpack(">" + "H" * count, pdu[6:6 + byte_count])
        with LOCK:
            HOLDING[address:address + count] = values
        return struct.pack(">BHH", function, address, count)

    return exception_pdu(function, 0x01)


class Handler(socketserver.BaseRequestHandler):
    def handle(self):
        while True:
            header = self.request.recv(7)
            if not header:
                return
            if len(header) != 7:
                return
            transaction, protocol, length, unit = struct.unpack(">HHHB", header)
            if protocol != 0 or length < 2:
                return
            body = b""
            while len(body) < length - 1:
                chunk = self.request.recv(length - 1 - len(body))
                if not chunk:
                    return
                body += chunk
            response_pdu = handle_pdu(unit, body)
            response = struct.pack(
                ">HHHB", transaction, 0, len(response_pdu) + 1, unit
            ) + response_pdu
            # Deterministic fault injection for the P1 regression binary.
            # blackhole keeps the TCP connection open and never replies, which
            # is the half-open case QModbusTcpClient cannot detect locally.
            if BLACKHOLE and body[0] in (0x03, 0x04):
                continue
            if SLOW_MS > 0:
                time.sleep(SLOW_MS / 1000.0)
            self.request.sendall(response)
            global REQUESTS
            with LOCK:
                REQUESTS += 1
                drop = DROP_AFTER > 0 and REQUESTS == DROP_AFTER
            if drop:
                self.request.shutdown(socket.SHUT_RDWR)
                return


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=15020)
    parser.add_argument("--drop-after", type=int, default=0)
    parser.add_argument("--slow-ms", type=int, default=0,
                        help="delay each response by this many milliseconds")
    parser.add_argument("--blackhole", action="store_true",
                        help="accept 0x03/0x04 but never reply")
    args = parser.parse_args()
    global DROP_AFTER, SLOW_MS, BLACKHOLE
    DROP_AFTER = args.drop_after
    SLOW_MS = args.slow_ms
    BLACKHOLE = args.blackhole
    with Server((args.host, args.port), Handler) as server:
        print(f"listening on {args.host}:{args.port}", flush=True)
        server.serve_forever()


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        sys.exit(0)
