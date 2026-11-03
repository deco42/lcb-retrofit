#!/usr/bin/env python3
import argparse
import collections
import os
import select
import signal
import struct
import sys
import time

import serial

BAUD = 9600
STATUS_INTERVAL_S = 10

# A command byte is the group (bits 4-6) and a number (bits 0-3), bit 7 marks an error
GROUP_STATUS, GROUP_ANGLE, GROUP_PARAMETER, GROUP_DNC, GROUP_SPRINGBACK = range(5)
ERROR_BIT = 0x80
ERROR_REPORT = 0xFF  # From the CNC

STATUS = bytes([0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x34, 0x00])
DNC_ACK = bytes([0x30, 0x05, 0x00, 0x37, 0x00, 0x03, 0x00, 0x4F, 0x4B, 0x20])  # Followed by the number of data bytes, 3 digits
BLOCK_LEN = {0: 8, 5: 6, 6: 4}
ERROR_REPORT_ACK = bytes([0x80])
INVALID = bytes([0xFF])
NOT_CALIBRATED = bytes([0x85])
SPRINGBACK_REPLY = bytes([0x40])
ANGLE_REPLY = 0x10  # Ored with the high 4 bits of the raw angle
BLOCK_ACK = 0x20  # Ored with the block number

NOT_CALIBRATED_PHASES = range(2, 6)
MEASURING_PHASES = range(1, 7)

# DNC block header: u16 count (0 ends the block), u16 type with '6' as low byte, u16 number of data words
DNC_HEADER_LEN = 6
DNC_TYPE_LOW = ord('6')


def ftdi_latency(device):
    name = os.path.basename(os.path.realpath(device))
    path = '/sys/bus/usb-serial/devices/%s/latency_timer' % name
    try:
        with open(path, 'w') as f:
            f.write('1')
        return 'FTDI latency timer set to 1 ms'
    except OSError as e:
        return 'FTDI latency timer not set (%s)' % e.strerror


class Slave:

    def __init__(self):
        self.state = 'cmd'
        self.block = 0
        self.buf = bytearray()
        self.p20 = self.p28 = self.p30 = 0
        self.set_angle10 = 0
        self.phase = None
        self.words = 0
        self.dnc = bytearray()
        self.counts = collections.Counter()
        self.blocks = collections.Counter()
        self.phases = collections.Counter()
        self.errors = 0
        self.dnc_messages = 0
        self.new_phase = None

    def byte(self, b):
        handler = getattr(self, 'state_' + self.state)
        return handler(b)

    def invalid(self):
        self.counts['invalid'] += 1
        self.errors += 1
        return INVALID, True

    def state_cmd(self, c):
        if c & ERROR_BIT:
            if c != ERROR_REPORT:
                return self.invalid()
            self.counts['error report (FF)'] += 1
            self.errors += 1
            return ERROR_REPORT_ACK, True

        group, n = (c >> 4) & 7, c & 0x0F
        if group == GROUP_STATUS:
            self.counts['status'] += 1
            return STATUS, True
        if group == GROUP_ANGLE:
            self.counts['angle request'] += 1
            self.phases[n] += 1
            if n != self.phase:
                self.new_phase = n
            self.phase = n
            if n in NOT_CALIBRATED_PHASES:
                return NOT_CALIBRATED, False
            return bytes([ANGLE_REPLY | (self.p20 >> 8) & 0x0F, self.p20 & 0xFF]), False
        if group == GROUP_PARAMETER:
            self.counts['parameter block'] += 1
            self.state, self.block, self.buf = 'param', n, bytearray()
            return b'', True
        if group == GROUP_DNC:
            self.counts['DNC block'] += 1
            self.state, self.buf = 'dnc_head', bytearray()
            return b'', True
        if group == GROUP_SPRINGBACK:
            self.counts['springback query'] += 1
            return SPRINGBACK_REPLY, True
        return self.invalid()

    def state_param(self, c):
        need = BLOCK_LEN.get(self.block)
        if need is None:
            self.blocks['unknown %d' % self.block] += 1
            self.errors += 1
            self.state = 'cmd'
            return INVALID, True

        self.buf.append(c)
        if len(self.buf) < need:
            return b'', True

        if self.block == 5:
            self.p20, self.p28, self.p30 = struct.unpack('<HHH', self.buf)
        elif self.block == 6:
            self.set_angle10 = struct.unpack('<H', self.buf[:2])[0]

        self.blocks[self.block] += 1
        self.state = 'cmd'
        return bytes([BLOCK_ACK | self.block]), True

    def state_dnc_head(self, c):
        self.buf.append(c)
        if len(self.buf) == 2 and struct.unpack('<H', self.buf)[0] == 0:
            self.state = 'cmd'
        elif len(self.buf) == 4 and self.buf[2] != DNC_TYPE_LOW:
            self.state = 'cmd'
        elif len(self.buf) == DNC_HEADER_LEN:
            self.words = struct.unpack('<H', self.buf[4:6])[0]
            self.dnc = bytearray()
            self.state = 'dnc_data' if self.words else 'cmd'

        return b'', True

    def state_dnc_data(self, c):
        self.dnc.append(c)
        if len(self.dnc) < 2 * self.words:
            return b'', True

        self.dnc_messages += 1
        self.state = 'cmd'
        return DNC_ACK + ('%03d' % (len(self.dnc) % 1000)).encode(), True


def main():
    ap = argparse.ArgumentParser(description='CNC link (NLR) test tool')
    ap.add_argument('device')
    ap.add_argument('--minutes', type=float, default=15)
    ap.add_argument('--log', default=time.strftime('com1_test_%Y%m%d_%H%M%S.log'))
    args = ap.parse_args()

    port = serial.Serial(args.device, BAUD, bytesize=8, parity='N', stopbits=1, timeout=0)
    log = open(args.log, 'w')
    t0 = time.monotonic()

    def out(line, console=True):
        text = '%9.3f %s' % (time.monotonic() - t0, line)
        log.write(text + '\n')
        if console:
            print(text, flush=True)

    out('com1_test on %s, %s, log %s' % (args.device, ftdi_latency(args.device), args.log))
    modem = True
    dtr = True

    try:
        port.dtr = True
        port.rts = True
    except OSError as e:
        modem = False
        out('WARNING: modem control lines not available (%s): no DTR acknowledge' % e)

    stop = []
    signal.signal(signal.SIGINT, lambda *a: stop.append(1))
    signal.signal(signal.SIGTERM, lambda *a: stop.append(1))
    slave = Slave()
    end = t0 + args.minutes * 60
    next_status = t0 + STATUS_INTERVAL_S
    reaction_max_ms = 0.0
    nbytes = 0
    while not stop and time.monotonic() < end:
        r, _, _ = select.select([port], [], [], 0.1)
        if r:
            for b in port.read(256):
                t_rx = time.monotonic()
                reply, ack = slave.byte(b)
                if reply:
                    port.write(reply)

                reaction = ''
                if ack and modem:
                    dtr = not dtr
                    port.dtr = dtr
                    reaction_ms = (time.monotonic() - t_rx) * 1000
                    reaction_max_ms = max(reaction_max_ms, reaction_ms)
                    reaction = ' DTR %d after %.3f ms' % (dtr, reaction_ms)

                nbytes += 1
                out('rx %02x -> tx %s%s' % (b, reply.hex(' ') or '-', reaction), console=False)

                if slave.new_phase is not None:
                    p = slave.new_phase
                    slave.new_phase = None
                    if p in MEASURING_PHASES:
                        out('*** phase %d: measuring phase - do not bend ***' % p)
                    else:
                        out('phase %d' % p)

        if time.monotonic() >= next_status:
            next_status += STATUS_INTERVAL_S
            out('status: %d bytes, phase %s, p20/p28/p30 %d/%d/%d, set angle %.1f, errors %d' % (
                nbytes, slave.phase, slave.p20, slave.p28, slave.p30, slave.set_angle10 / 10, slave.errors))

    port.close()

    out('summary: %d bytes received' % nbytes)
    for k, v in sorted(slave.counts.items()):
        out('  %-20s %d' % (k, v))
    out('  parameter blocks: %s' % (', '.join('%s x%d' % (k, v) for k, v in sorted(slave.blocks.items(), key=str)) or 'none'))
    out('  phases seen: %s' % (', '.join('%d x%d' % (k, v) for k, v in sorted(slave.phases.items())) or 'none'))
    out('  DNC messages: %d, errors: %d, max DTR reaction %.3f ms' % (slave.dnc_messages, slave.errors, reaction_max_ms))
    ok = slave.errors == 0 and nbytes > 0
    out('%s - the CNC must show 180 deg under WINKEL/IST' % ('PASS: the line ran without errors' if ok else
                                                             'FAIL: errors or no bytes received'))
    log.close()
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
