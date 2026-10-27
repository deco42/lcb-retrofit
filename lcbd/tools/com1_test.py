#!/usr/bin/env python3
"""
Plays the slave on the serial link to the Cybelec DNC 1200:

Answers status requests, parameter blocks and DNC blocks, answers angle
requests with 180.0 degrees (raw value = p20) in phases 0, 1, 6, 7 (and 8-15)
and with 0x85 ("not calibrated") in phases 2-5

Every byte is logged with its time and the DTR reaction time, a status line is printed
every 10 s and a summary at the end.

usage: com1_test.py DEVICE [--minutes N] [--log FILE]
"""
import argparse
import collections
import os
import select
import signal
import struct
import sys
import time

import serial

DNC_ACK = bytes([0x30, 0x05, 0x00, 0x37, 0x00, 0x03, 0x00, 0x4F, 0x4B, 0x20])
STATUS = bytes([0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x34, 0x00])
BLOCK_LEN = {0: 8, 5: 6, 6: 4}


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

    def state_cmd(self, c):
        if c & 0x80:
            if c == 0xFF:
                self.counts['error report (FF)'] += 1
                self.errors += 1
                return bytes([0x80]), True

            self.counts['invalid'] += 1
            self.errors += 1
            return bytes([0xFF]), True

        g, n = (c >> 4) & 7, c & 0x0F
        if g == 0:
            self.counts['status'] += 1
            return STATUS, True
        elif g == 1:
            self.counts['angle request'] += 1
            self.phases[n] += 1
            if n != self.phase:
                self.new_phase = n
            self.phase = n
            if 2 <= n <= 5:
                return bytes([0x85]), False
            return bytes([0x10 | (self.p20 >> 8) & 0x0F, self.p20 & 0xFF]), False
        elif g == 2:
            self.counts['parameter block'] += 1
            self.state, self.block, self.buf = 'param', n, bytearray()
            return b'', True
        elif g == 3:
            self.counts['DNC block'] += 1
            self.state, self.buf = 'dnc_head', bytearray()
            return b'', True
        elif g == 4:
            self.counts['springback query'] += 1
            return bytes([0x40]), True

        self.counts['invalid'] += 1
        self.errors += 1
        return bytes([0xFF]), True

    def state_param(self, c):
        need = BLOCK_LEN.get(self.block)
        if need is None:
            self.blocks['unknown %d' % self.block] += 1
            self.errors += 1
            self.state = 'cmd'
            return bytes([0xFF]), True

        self.buf.append(c)
        if len(self.buf) < need:
            return b'', True

        b = self.buf
        if self.block == 5:
            self.p20, self.p28, self.p30 = struct.unpack('<HHH', b)
        elif self.block == 6:
            self.set_angle10 = struct.unpack('<H', b[:2])[0]

        self.blocks[self.block] += 1
        self.state = 'cmd'
        return bytes([0x20 | self.block]), True

    def state_dnc_head(self, c):
        self.buf.append(c)
        if len(self.buf) == 2 and struct.unpack('<H', self.buf)[0] == 0:
            self.state = 'cmd'
        elif len(self.buf) == 4 and self.buf[2] != ord('6'):
            self.state = 'cmd'
        elif len(self.buf) == 6:
            self.words = struct.unpack('<H', self.buf[4:6])[0]
            self.dnc = bytearray()
            self.state = 'dnc_data' if self.words else 'cmd'

        return b'', True

    def state_dnc_data(self, c):
        self.dnc.append(c)
        if len(self.dnc) < 2 * self.words:
            return b'', True

        n = len(self.dnc)
        self.dnc_messages += 1
        self.state = 'cmd'
        return DNC_ACK + ('%03d' % (n % 1000)).encode(), True


def main():
    ap = argparse.ArgumentParser(description='CNC link (NLR) test tool')
    ap.add_argument('device')
    ap.add_argument('--minutes', type=float, default=15)
    ap.add_argument('--log', default=time.strftime('com1_test_%Y%m%d_%H%M%S.log'))
    args = ap.parse_args()

    port = serial.Serial(args.device, 9600, bytesize=8, parity='N', stopbits=1, timeout=0)
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
    next_status = t0 + 10
    reaction_max = 0.0
    nbytes = 0
    while not stop and time.monotonic() < end:
        r, _, _ = select.select([port], [], [], 0.1)
        if r:
            data = port.read(256)
            for b in data:
                t_rx = time.monotonic()
                reply, ack = slave.byte(b)
                if reply:
                    port.write(reply)

                reaction = ''
                if ack and modem:
                    dtr = not dtr
                    port.dtr = dtr
                    dt = (time.monotonic() - t_rx) * 1000
                    reaction_max = max(reaction_max, dt)
                    reaction = ' DTR %d after %.3f ms' % (dtr, dt)

                nbytes += 1
                out('rx %02x -> tx %s%s' % (b, reply.hex(' ') or '-', reaction), console=False)

                if slave.new_phase is not None:
                    p = slave.new_phase
                    slave.new_phase = None
                    if 1 <= p <= 6:
                        out('*** phase %d: measuring phase - do not bend ***' % p)
                    else:
                        out('phase %d' % p)

        if time.monotonic() >= next_status:
            next_status += 10
            out('status: %d bytes, phase %s, p20/p28/p30 %d/%d/%d, set angle %.1f, errors %d' % (
                nbytes, slave.phase, slave.p20, slave.p28, slave.p30, slave.set_angle10 / 10, slave.errors))

    port.close()

    out('summary: %d bytes received' % nbytes)
    for k, v in sorted(slave.counts.items()):
        out('  %-20s %d' % (k, v))
    out('  parameter blocks: %s' % (', '.join('%s x%d' % (k, v) for k, v in sorted(slave.blocks.items(), key=str)) or 'none'))
    out('  phases seen: %s' % (', '.join('%d x%d' % (k, v) for k, v in sorted(slave.phases.items())) or 'none'))
    out('  DNC messages: %d, errors: %d, max DTR reaction %.3f ms' % (slave.dnc_messages, slave.errors, reaction_max))
    ok = slave.errors == 0 and nbytes > 0
    out('%s - the CNC must show 180 deg under WINKEL/IST' % ('PASS: the line ran without errors' if ok else
                                                             'FAIL: errors or no bytes received'))
    log.close()
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
