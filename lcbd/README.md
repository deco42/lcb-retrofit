# lcbd

Laser angle measurement of the press brake: drives the two measuring units, evaluates the two laser line sensors,
sends the bend angle to the Cybelec DNC 1200 over the serial NLR link.

## Threads

| Thread | Scheduling | Interface | Modules |
|---|---|---|---|
| I/O | 1 ms, SCHED_FIFO 80 | `ecat0`: EK1100, EL1819, EL2809, EL1502 | `hal`, `ecat`/`sim`, `io`, `control` (10 ms), `sequences` (100 ms) |
| link | SCHED_FIFO 70 | `/dev/ttyNLR`, DTR ack per byte | `link`, `nlr` |
| sensor rear / front | normal | TCP 192.168.123.248 / .249:3000 | `sensor_client`, `sensorproto`, `evaluator`, `station` |
| web | normal | HTTP :80 | `web`, `httputil`, embedded `web/` |

## Sources

| File | Content |
|---|---|
| `src/evaluator.c` | leg angle from one sensor profile |
| `src/station.c` | both legs combined, held angle, smoothing |
| `src/nlr.c` | CNC link protocol, angle scaling, parameter and DNC blocks |
| `src/sensorproto.c` | sensor commands, 2048-byte frames |
| `src/io.c` | debounce, counters, drives, cylinder, lamps |
| `src/control.c`, `src/sequences.c` | controller, reference, cylinder and die sequences, service commands |
| `src/config.c`, `src/persist.c` | data files, saving (under overlayroot also to `/media/root-ro`) |
| `src/main.c` | CLI, threads, systemd watchdog |
| `src/lcb_test.c` | commissioning tool, `lcb_test IFACE [--read-only]` |
| `tools/replay.c` | `lcb_replay CAPTURE.pcap [--csv]`, angle per frame from a sensor capture |
| `tools/com1_test.py` | CNC link test, answers only 180° or `85` |

## Build

```
cmake -S . -B build
cmake --build build -j
```

| Option | Effect |
|---|---|
| `-DSOEM_URL=FILE` | SOEM v2.0.1 archive instead of the GitHub download (SHA256 checked) |
| `-DTESTDATA=DIR` | test data folder, default [`../testdata/`](../testdata/) |
| `-DLCBD_SANITIZE=ON` | AddressSanitizer + UndefinedBehaviorSanitizer |

## Test

```
ctest --test-dir build
```

`../testdata/` holds the CNC link vectors. The sensor recordings (`bend_090`, `bend_135`, `synthetic`, ~210 MB, with
the results of the original box) are not in the repository. Without them in `TESTDATA` the frame and replay tests are
skipped.

## Run

```
sudo lcbd [--ecat IFACE] [--com DEVICE] [--sensor IP[:PORT] --sensor IP[:PORT]] [--sim]
          [--web [ADDRESS:]PORT | --web 0] [--data DIR]
```

| Option | Default |
|---|---|
| `--ecat` | `ecat0` |
| `--com` | `/dev/ttyNLR` |
| `--sensor` | `192.168.123.248:3000` (rear), `192.168.123.249:3000` (front) |
| `--web` | `0.0.0.0:80`, `0` = off |
| `--data` | `/etc/lcbd` |
| `--sim` | machine model instead of EtherCAT, stdin line `E5` presses input E5 for 300 ms |

| Data file | Content |
|---|---|
| `setup.dat`, `sensor.dat`, `die.dat` | settings, examples in [`../configs/`](../configs/) |
| `web.pass` | first line = password of user `service`, missing = service page read only |

## Install

- `cmake --install build`: `lcbd`, `lcb_test` to `/usr/local/bin`
- systemd unit: root, `Restart=always`, `WatchdogSec=15`, `LimitRTPRIO=99`, `LimitMEMLOCK=infinity`
- `WATCHDOG=1` every 1 s, stops when a worker loop makes no progress for 6 s
- log: `journalctl -u lcbd`
