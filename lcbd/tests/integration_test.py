#!/usr/bin/env python3
import base64
import json
import math
import os
import select
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.parse
import urllib.request

# Sensor commands of the default sensor.dat
CONNECT = bytes.fromhex('15 81 00 fe 01 87 06 90 07 83 1c')
START_REF = bytes.fromhex('0b 81 15 80')
START_SCAN = bytes.fromhex('15 80')
STOP_SCAN = bytes.fromhex('0b 80 15 81 00 fe 01 87 06 90 07 83')

# CNC link: parameter block 5 sets p20 (raw angle of 180 degrees) and p28 (raw angle of 90 degrees)
P20, P28 = 1000, 2000
STATUS_REQUEST = b'\x00'
STATUS_REPLY = bytes.fromhex('00 01 00 00 00 00 00 34 00')
BLOCK5 = bytes.fromhex('25 e8 03 d0 07 00 00')
BLOCK5_ACK = b'\x25'
P20_REPLY = bytes.fromhex('13 e8')
ANGLE_REPLY_LEN = 2

# Machine model of the synthetic frames: rows of points along the die flank from the die edge R and along the leg
REF_X, REF_Y = 1500, 2000
DIE_ANGLE = -60
POINT_STEP = 12
DIE_POINTS, LEG_POINTS = 120, 70
LEG_OFFSET_Y = 100
POINT_QUALITY = 100
NO_LEG, LEG_ANGLE = -1, 30

FRAME_SIZE = 2048
FRAME_RECORDS = 290
FRAME_POINTS_OFFSET = 0x42
FRAME_RECORD_SIZE = 5
FRAME_X_SCALE = 1.5

HISTORY_MAX = 1024  # Angle.txt lines (shared.h)

results = []


def check(name, cond, detail=''):
    results.append(bool(cond))
    print('%s %s%s' % ('PASS' if cond else 'FAIL', name, (' (%s)' % detail) if detail else ''), flush=True)


def wait(cond, timeout_s, step_s=0.05):
    end = time.time() + timeout_s
    while time.time() < end:
        v = cond()
        if v:
            return v
        time.sleep(step_s)
    return cond()


def angle_request(phase):
    return bytes([0x10 | phase])


def cnc_exchange(master, data, reply_len, byte_gap_s=0.002, timeout_s=1.0):
    out = b''
    for b in data:
        os.write(master, bytes([b]))
        time.sleep(byte_gap_s)
    end = time.time() + timeout_s
    while len(out) < reply_len and time.time() < end:
        if select.select([master], [], [], 0.05)[0]:
            out += os.read(master, 256)
    return out


def sim_frame(leg_angle):
    die = math.radians(DIE_ANGLE)
    if leg_angle == NO_LEG:
        pts = [(REF_X - POINT_STEP * math.cos(die), REF_Y - POINT_STEP * math.sin(die))]
    else:
        leg = math.radians(DIE_ANGLE + leg_angle)
        pts = [(REF_X - i * POINT_STEP * math.cos(leg), REF_Y + LEG_OFFSET_Y - i * POINT_STEP * math.sin(leg))
               for i in range(LEG_POINTS, 0, -1)]
    pts.append((REF_X, REF_Y))
    pts += [(REF_X + i * POINT_STEP * math.cos(die), REF_Y + i * POINT_STEP * math.sin(die))
            for i in range(1, DIE_POINTS + 1)]
    frame = bytearray(FRAME_SIZE)
    for i, (x, y) in enumerate(pts):
        raw_x, raw_y = round(x * FRAME_X_SCALE), round(y)
        start = FRAME_POINTS_OFFSET + FRAME_RECORD_SIZE * i
        frame[start:start + FRAME_RECORD_SIZE] = bytes([raw_x & 0x7F, raw_x >> 7, raw_y & 0x7F, raw_y >> 7, POINT_QUALITY])
    return bytes(frame)


FRAME_PERIOD_S = 0.02
TCP_PIECE = 1460


class SensorServer(threading.Thread):

    def __init__(self):
        super().__init__(daemon=True)
        self.sock = socket.socket()
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(('127.0.0.1', 0))
        self.sock.listen(1)
        self.port = self.sock.getsockname()[1]
        self.frames = {'ref': sim_frame(NO_LEG), 'leg': sim_frame(LEG_ANGLE)}
        self.mode = 'ref'
        self.connections = []  # Received bytes per connection
        self.drop = False

    def run(self):
        while True:
            conn, _ = self.sock.accept()
            conn.setblocking(False)
            rx = bytearray()
            self.connections.append(rx)
            next_s = time.time()
            try:
                while not self.drop:
                    if select.select([conn], [], [], 0)[0]:
                        data = conn.recv(4096)
                        if not data:
                            break
                        rx += data
                    if time.time() >= next_s:
                        frame = self.frames[self.mode]
                        conn.sendall(frame[:TCP_PIECE])
                        conn.sendall(frame[TCP_PIECE:])
                        next_s += FRAME_PERIOD_S
                    time.sleep(0.002)
            except OSError:
                pass
            conn.close()
            self.drop = False


class Lcbd:
    def __init__(self, exe, args):
        self.p = subprocess.Popen([exe] + args, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT, text=True, bufsize=1)
        self.lines = []
        threading.Thread(target=self._reader, daemon=True).start()

    def _reader(self):
        for line in self.p.stdout:
            self.lines.append(line.rstrip('\n'))

    def log_has(self, text):
        return any(text in line for line in self.lines)

    def press(self, n):
        self.p.stdin.write('E%d\n' % n)
        self.p.stdin.flush()


class Http:
    def __init__(self, port):
        self.base = 'http://127.0.0.1:%d' % port

    def get(self, path):
        with urllib.request.urlopen(self.base + path, timeout=5) as r:
            return r.status, dict(r.headers), r.read()

    def json(self, path):
        return json.loads(self.get(path)[2])

    def post(self, path, fields, password=None):
        data = urllib.parse.urlencode(fields).encode()
        req = urllib.request.Request(self.base + path, data=data, method='POST')
        if password is not None:
            req.add_header('Authorization', 'Basic ' + base64.b64encode(('service:' + password).encode()).decode())
        try:
            with urllib.request.urlopen(req, timeout=5) as r:
                return r.status, dict(r.headers), json.loads(r.read())
        except urllib.error.HTTPError as e:
            return e.code, dict(e.headers), json.loads(e.read())


def free_port():
    s = socket.socket()
    s.bind(('127.0.0.1', 0))
    port = s.getsockname()[1]
    s.close()
    return port


def startup_failures(exe, data):
    r = subprocess.run([exe, '--bogus'], capture_output=True, text=True, timeout=5)
    check('wrong option: usage on stderr, exit code 2', r.returncode == 2 and 'usage' in r.stderr)
    r = subprocess.run([exe, '--sensor', '10.0.0.1'], capture_output=True, text=True, timeout=5)
    check('only one --sensor: exit code 2', r.returncode == 2)
    r = subprocess.run([exe, '--ecat', 'lcbdnone0', '--web', '0', '--data', data], capture_output=True, text=True,
                       timeout=20)
    check('EtherCAT cannot be opened: reason logged, exit code 1', r.returncode == 1 and 'EtherCAT on lcbdnone0' in r.stdout,
          r.stdout.strip().splitlines()[-1] if r.stdout.strip() else '')


def missing_devices(exe, data):
    port = free_port()
    d = Lcbd(exe, ['--sim', '--com', '/nonexistent/ttyNLR', '--sensor', '127.0.0.1:%d' % free_port(), '--sensor',
                   '127.0.0.1:%d' % free_port(), '--web', '127.0.0.1:%d' % port, '--data', data])
    time.sleep(4.5)
    ok = wait(lambda: Http(port).json('/api/status')['ctl']['state'] == 'idle', 5)
    com = sum('CNC link: cannot open' in line for line in d.lines)
    rear = sum('sensor rear' in line and 'not reachable' in line for line in d.lines)
    check('serial device missing / sensor unreachable: logged once, other parts run', ok and com == 1 and rear == 1,
          'CNC link lines %d, rear sensor lines %d' % (com, rear))
    d.p.send_signal(signal.SIGTERM)
    d.p.wait(timeout=6)


def main():
    exe = sys.argv[1]
    data = tempfile.mkdtemp(prefix='lcbd-it-')
    startup_failures(exe, data)
    missing_devices(exe, data)
    sensors = [SensorServer(), SensorServer()]
    for s in sensors:
        s.start()
    master, slave = os.openpty()
    web_port = free_port()
    notify = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    notify.bind(os.path.join(data, 'notify.sock'))
    os.environ['NOTIFY_SOCKET'] = os.path.join(data, 'notify.sock')
    d = Lcbd(exe, ['--sim', '--com', os.ttyname(slave), '--sensor', '127.0.0.1:%d' % sensors[0].port,
                   '--sensor', '127.0.0.1:%d' % sensors[1].port, '--web', '127.0.0.1:%d' % web_port, '--data', data])
    http = Http(web_port)
    try:
        run(d, http, sensors, master, data)
        notify.setblocking(False)
        pings = 0
        while select.select([notify], [], [], 0)[0]:
            pings += notify.recv(64) == b'WATCHDOG=1'
        check('systemd watchdog notified about once per second', pings >= 5, '%d notifications' % pings)
    finally:
        if d.p.poll() is None:
            d.p.kill()
        print('\n'.join('    | ' + line for line in d.lines[-25:]))
        shutil.rmtree(data, ignore_errors=True)
    print('%d checks, %d failed' % (len(results), results.count(False)))
    return 0 if all(results) else 1


def run(d, http, sensors, master, data):
    ok = wait(lambda: all(s.connections and s.connections[0][:len(CONNECT)] == CONNECT for s in sensors), 8)
    check('both sensors got the connect bytes', ok)
    st = wait(lambda: (lambda j: j if j['ctl']['state'] == 'idle' and not j['ctl']['cylinder_up'] else None)(
        http.json('/api/status')), 12)
    check('start: units home, cylinder down, controller idle', st)

    # Service page: every window's data
    status, headers, body = http.get('/')
    check('page served', status == 200 and b'lcbd' in body and headers.get('Cache-Control') == 'no-store')
    check('script and style served', http.get('/app.js')[0] == 200 and http.get('/style.css')[0] == 200)
    j = http.json('/api/status')
    check('status has all windows', all(k in j for k in ('io', 'ctl', 'link', 'sensors', 'station')) and
          len(j['sensors']) == 2 and j['sensors'][0]['connected'] and 'dnc_text' in j['link'])
    prof = http.json('/api/profile?s=1')
    check('sensor profile', len(prof['points']) == 2 + DIE_POINTS and 'res' in prof, '%d points' % len(prof['points']))
    check('angle history', 'entries' in http.json('/api/history'))
    check('log window', any('sensor rear' in line for line in http.json('/api/log')['lines']))
    check('setup window', http.json('/api/setup')['file']['MV_METHOD'] == 'MV_REF')
    check('die table window', http.json('/api/dies')['dies'] == [])
    pts = http.get('/dl/Points.txt')[2].decode().splitlines()
    check('Points.txt', len(pts) == 1 + FRAME_RECORDS and pts[0].startswith('BACKX_0, BACKZ_0') and
          pts[2].startswith('%d.00, %d.00' % (REF_X, REF_Y)), pts[2] if len(pts) > 2 else '')
    ang = http.get('/dl/Angle.txt')[2].decode().splitlines()
    check('Angle.txt', len(ang) == HISTORY_MAX and ang[-1].startswith('NLR: '), ang[-1] if ang else '')

    # Refusals and a successful command
    s, h, r = http.post('/api/cmd', {'op': 'on', 'n': 7, 'enable': 1}, 'x')
    check('POST refused without web.pass (403)', s == 403 and not r['ok'], r['text'])
    with open(os.path.join(data, 'web.pass'), 'w') as f:
        f.write('secret\n')
    s, h, r = http.post('/api/cmd', {'op': 'on', 'n': 7, 'enable': 1})
    check('POST refused without login (401, no browser login dialog)', s == 401 and 'WWW-Authenticate' not in h, r['text'])
    s, h, r = http.post('/api/cmd', {'op': 'on', 'n': 7, 'enable': 1}, 'wrong')
    check('POST refused with wrong login (401)', s == 401)
    s, h, r = http.post('/api/cmd', {'op': 'on', 'n': 7}, 'secret')
    check('output refused without "enable switching"', s == 200 and not r['ok'], r['text'])
    s, h, r = http.post('/api/cmd', {'op': 'reset', 'unit': 0}, 'secret')
    check('counter reset refused without "enable switching"', s == 200 and not r['ok'], r['text'])
    s, h, r = http.post('/api/cmd', {'op': 'on', 'n': 7, 'enable': 1}, 'secret')
    lamp = wait(lambda: http.json('/api/status')['io']['out'] & 0x40, 2)  # Output bit of A7
    check('command A7 on', s == 200 and r['ok'] and lamp, r['text'])
    http.post('/api/cmd', {'op': 'off', 'n': 7, 'enable': 1}, 'secret')
    s, h, r = http.post('/api/cmd', {'op': 'on', 'n': 6, 'enable': 1}, 'secret')
    check('A6 refused while A5 is on', s == 200 and not r['ok'] and 'A5' in r['text'], r['text'])
    fields = {'MV_METHOD': 'MV_REF', 'MOVE_DOWN_START': 'TRUE', 'USE_SECURE_ANGLE': 'FALSE', 'SB_TIMEOUT': '12',
              'SB_MEANTILT': 'x', 'LE_POINTS_FIXL3': '60', 'LE_POINTS_MIN': '50', 'LE_COVER': '99',
              'LE_TOLERANCE': 'abc', 'LE_LINE_MODE': 'LM_EXTENDED'}
    s, h, r = http.post('/api/setup', fields, 'secret')
    saved = open(os.path.join(data, 'setup.dat')).read() if os.path.exists(os.path.join(data, 'setup.dat')) else ''
    check('setup save', s == 200 and r['ok'] and saved ==
          'MV_METHOD=MV_REF\nMOVE_DOWN_START=TRUE\nUSE_SECURE_ANGLE=FALSE\nUSE_TURN=FALSE\nSB_TIMEOUT=12\n'
          'SB_MEANTILT=7\nLE_POINTS_FIXL3=60\nLE_POINTS_MIN=50\nLE_COVER=99\nLE_TOLERANCE=0.4\n'
          'LE_LINE_MODE=LM_EXTENDED\n', r['text'])
    s, h, r = http.post('/api/setup', {'MV_METHOD': 'MV_REF\nUSE_TURN=TRUE'}, 'secret')
    unchanged = open(os.path.join(data, 'setup.dat')).read() == saved
    check('setup with a line break refused', s == 200 and not r['ok'] and 'line break' in r['text'] and unchanged,
          r['text'])
    check('service commands logged', d.log_has('service: output A7 on') and d.log_has('refused'))

    # Reference via E6 / E5 with synthetic frames
    d.press(6)
    up = wait(lambda: (lambda c: c['state'] == 'idle' and c['cylinder_up'])(http.json('/api/status')['ctl']), 6)
    check('E6: cylinder up', up)
    d.press(5)
    ref = wait(lambda: (lambda c: c['state'] == 'idle' and c['referenced'])(http.json('/api/status')['ctl']), 6)
    check('E5: referenced', ref)
    check('sensors got "start reference"', all(START_REF in bytes(s.connections[-1]) for s in sensors))

    # Angle replies over the CNC link
    check('CNC: status', cnc_exchange(master, STATUS_REQUEST, len(STATUS_REPLY)) == STATUS_REPLY)
    check('CNC: parameter block 5', cnc_exchange(master, BLOCK5, len(BLOCK5_ACK)) == BLOCK5_ACK)
    r0 = cnc_exchange(master, angle_request(0), ANGLE_REPLY_LEN)
    check('CNC: phase 0 reply = p20', r0 == P20_REPLY, r0.hex())
    cnc_exchange(master, angle_request(1), ANGLE_REPLY_LEN)
    scan = wait(lambda: all(bytes(s.connections[-1]).endswith(START_SCAN) for s in sensors), 2)
    check('phase 1: scan started (sensors got "start scan")', scan)
    for s in sensors:
        s.mode = 'leg'

    def angle(raw):
        return 180 - (raw - P20) * 90 / (P28 - P20)

    def measured():
        # The link reports the result of the last completed pair: right after "start scan" this can still be
        # the pre-scan value (0 deg), so wait for a measured angle
        r = cnc_exchange(master, angle_request(2), ANGLE_REPLY_LEN)
        raw = ((r[0] & 0x0F) << 8 | r[1]) if len(r) == ANGLE_REPLY_LEN and r[0] & 0xF0 == 0x10 else None
        return raw if raw is not None and 20 < angle(raw) < 180 else None
    raw = wait(measured, 5, 0.1)
    st = http.json('/api/status')
    measured_angle = angle(raw) if raw else None
    check('phase 2: measured angle replied',
          raw and 20 <= measured_angle < 180 and abs(measured_angle - st['link']['angle']) < 0.2,
          'raw %s = %.2f deg, station %.2f deg' % (raw, measured_angle or 0, st['station']['combined']))
    cnc_exchange(master, angle_request(7), ANGLE_REPLY_LEN)
    stop = wait(lambda: all(bytes(s.connections[-1]).endswith(STOP_SCAN) for s in sensors), 2)
    check('phase 7: scan stopped', stop)
    cnc_exchange(master, angle_request(0), ANGLE_REPLY_LEN)

    # Reconnect of a sensor
    n = len(sensors[0].connections)
    sensors[0].drop = True
    again = wait(lambda: len(sensors[0].connections) > n and sensors[0].connections[-1][:len(CONNECT)] == CONNECT, 8)
    check('sensor reconnect with connect bytes', again and d.log_has('sensor rear: disconnected'))

    # Clean exit
    d.p.send_signal(signal.SIGTERM)
    try:
        code = d.p.wait(timeout=6)
    except subprocess.TimeoutExpired:
        code = None
    time.sleep(0.2)
    check('SIGTERM: exit code 0, outputs off', code == 0 and d.log_has('outputs off') and d.log_has('stopped'),
          'exit code %s' % code)


if __name__ == '__main__':
    sys.exit(main())
