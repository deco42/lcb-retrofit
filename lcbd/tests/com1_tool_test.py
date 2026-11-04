#!/usr/bin/env python3
import os
import subprocess
import sys
import tempfile

from integration_test import (ANGLE_REPLY_LEN, BLOCK5, BLOCK5_ACK, P20_REPLY, STATUS_REPLY, STATUS_REQUEST,
                              angle_request, check, cnc_exchange, results)

NOT_CALIBRATED = b'\x85'
SPRINGBACK_QUERY, SPRINGBACK_REPLY = b'\x41', b'\x40'
DNC_BLOCK = bytes.fromhex('30 01 00 36 00 02 00') + b'1-01'
DNC_REPLY = bytes.fromhex('30 05 00 37 00 03 00 4f 4b 20') + b'004'


def main():
    tool = sys.argv[1]
    master, slave = os.openpty()
    log = tempfile.NamedTemporaryFile(suffix='.log', delete=False).name
    p = subprocess.Popen([sys.executable, tool, os.ttyname(slave), '--minutes', '0.08', '--log', log],
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    first = p.stdout.readline()  # Started, the port is configured

    def exchange(data, reply_len):
        return cnc_exchange(master, data, reply_len, byte_gap_s=0.003, timeout_s=2.0)

    check('status', exchange(STATUS_REQUEST, len(STATUS_REPLY)) == STATUS_REPLY)
    check('parameter block 5', exchange(BLOCK5, len(BLOCK5_ACK)) == BLOCK5_ACK)
    check('phase 0: 180 deg = p20', exchange(angle_request(0), ANGLE_REPLY_LEN) == P20_REPLY)
    check('phase 3: not calibrated', exchange(angle_request(3), len(NOT_CALIBRATED)) == NOT_CALIBRATED)
    check('phase 6: 180 deg = p20', exchange(angle_request(6), ANGLE_REPLY_LEN) == P20_REPLY)
    check('springback query', exchange(SPRINGBACK_QUERY, len(SPRINGBACK_REPLY)) == SPRINGBACK_REPLY)
    check('DNC block', exchange(DNC_BLOCK, len(DNC_REPLY)) == DNC_REPLY)
    output = first + p.communicate(timeout=30)[0]
    print(output)
    check('measuring phase warning', 'phase 3: measuring phase - do not bend' in output)
    check('summary and pass line', 'PASS: the line ran without errors' in output and p.returncode == 0)
    check('byte log', 'rx %s -> tx %s' % (angle_request(3).hex(), NOT_CALIBRATED.hex()) in open(log).read())
    os.unlink(log)
    return 0 if all(results) else 1


if __name__ == '__main__':
    sys.exit(main())
