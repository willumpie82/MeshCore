#!/usr/bin/env python3
"""
Provision WiFi credentials over serial using the Improv Wi-Fi protocol
(https://www.improv-wifi.com/serial/), for MeshCore firmware built with
IMPROV_WIFI_SERIAL=1 (see src/helpers/esp32/ImprovWiFiSerial.h).

No firmware rebuild/reflash needed to change networks - credentials are
sent once over USB serial and persisted to NVS on the device.

Usage:
    pip3 install pyserial
    python3 tools/improv_provision.py --list
    python3 tools/improv_provision.py /dev/ttyUSB0 "MySSID" "MyPassword"
"""
import argparse
import struct
import sys
import time

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    print("Missing pyserial. Install with: pip3 install pyserial")
    sys.exit(1)

# Improv Wi-Fi wire format constants
TYPE_CURRENT_STATE = 0x01
TYPE_ERROR_STATE = 0x02
TYPE_RPC = 0x03
TYPE_RPC_RESPONSE = 0x04

CMD_WIFI_SETTINGS = 0x01
CMD_GET_CURRENT_STATE = 0x02
CMD_GET_DEVICE_INFO = 0x03

STATE_NAMES = {
    0x00: "stopped",
    0x01: "awaiting authorization",
    0x02: "authorized (ready to provision)",
    0x03: "provisioning",
    0x04: "provisioned",
}
ERROR_NAMES = {
    0x00: "none",
    0x01: "invalid RPC packet",
    0x02: "unknown RPC command",
    0x03: "unable to connect to WiFi",
    0x04: "not authorized",
}


def build_packet(ptype: int, payload: bytes) -> bytes:
    hdr = b"IMPROV" + bytes([1, ptype, len(payload)])
    checksum = (sum(hdr) + sum(payload)) & 0xFF
    return hdr + payload + bytes([checksum])


def build_rpc(cmd: int, strings: list[str]) -> bytes:
    body = b""
    for s in strings:
        b = s.encode()
        body += bytes([len(b)]) + b
    payload = bytes([cmd, len(body)]) + body
    return build_packet(TYPE_RPC, payload)


def read_packet(ser, timeout=5.0):
    """Read one IMPROV-framed packet, resyncing on the 'IMPROV' magic if needed."""
    deadline = time.time() + timeout
    buf = b""
    while time.time() < deadline:
        chunk = ser.read(64)
        if chunk:
            buf += chunk
        idx = buf.find(b"IMPROV")
        if idx == -1:
            continue
        buf = buf[idx:]
        if len(buf) < 9:
            continue
        data_len = buf[8]
        end = 9 + data_len + 1
        if len(buf) < end:
            continue
        return buf[:end]
    return None


def parse_rpc_response(payload: bytes) -> list[str]:
    rpc_len = payload[1]
    data = payload[2:2 + rpc_len]
    strings, i = [], 0
    while i < len(data):
        slen = data[i]
        strings.append(data[i + 1:i + 1 + slen].decode(errors="replace"))
        i += 1 + slen
    return strings


def handle_packet(pkt: bytes) -> bool:
    """Print a human-readable summary of a received packet. Returns True if provisioned."""
    ptype = pkt[7]
    data_len = pkt[8]
    payload = pkt[9:9 + data_len]

    if ptype == TYPE_CURRENT_STATE:
        state = payload[0]
        print(f"  state: {STATE_NAMES.get(state, hex(state))}")
        return state == 0x04
    elif ptype == TYPE_ERROR_STATE:
        err = payload[0]
        if err != 0:
            print(f"  error: {ERROR_NAMES.get(err, hex(err))}")
    elif ptype == TYPE_RPC_RESPONSE:
        strings = parse_rpc_response(payload)
        print(f"  response: {strings}")
    return False


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("port", nargs="?", help="Serial port, e.g. /dev/ttyUSB0 or COM5")
    ap.add_argument("ssid", nargs="?", help="WiFi network name")
    ap.add_argument("password", nargs="?", default="", help="WiFi password (omit for open networks)")
    ap.add_argument("--list", action="store_true", help="List available serial ports and exit")
    ap.add_argument("--baud", type=int, default=115200)
    args = ap.parse_args()

    if args.list or not args.port:
        print("Available serial ports:")
        for p in list_ports.comports():
            print(f"  {p.device}  ({p.description})")
        if not args.port:
            ap.print_usage()
        return

    if not args.ssid:
        ap.error("ssid is required unless using --list")

    ser = serial.Serial(args.port, baudrate=args.baud, timeout=0.5, dsrdtr=False, rtscts=False)
    time.sleep(0.3)
    ser.reset_input_buffer()

    print("--- Checking current state ---")
    ser.write(build_rpc(CMD_GET_CURRENT_STATE, []))
    pkt = read_packet(ser, timeout=3.0)
    if pkt is None:
        print("No response. Is this device running Improv-enabled MeshCore firmware "
              "(built with IMPROV_WIFI_SERIAL=1) and currently awaiting provisioning?")
        ser.close()
        sys.exit(1)
    handle_packet(pkt)

    print(f"\n--- Provisioning WiFi: {args.ssid!r} ---")
    ser.write(build_rpc(CMD_WIFI_SETTINGS, [args.ssid, args.password]))

    provisioned = False
    deadline = time.time() + 40.0  # WiFi.begin() can occasionally take a while to associate
    while time.time() < deadline:
        pkt = read_packet(ser, timeout=deadline - time.time())
        if pkt is None:
            break
        if handle_packet(pkt):
            provisioned = True
            break

    ser.close()

    if provisioned:
        print("\nProvisioned successfully.")
    else:
        print("\nProvisioning did not complete - check the SSID/password and that the "
              "device is in range of the network.")
        sys.exit(1)


if __name__ == "__main__":
    main()
