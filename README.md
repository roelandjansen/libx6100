# libx6100

Open-source C control library for the Xiegu X6100 baseband (STM32), used from the
Allwinner R16 running Linux. Written from scratch from the stock Xiegu
APP 1.2.0 / BASE 1.1.9 interface; contains no R1CBU, Aether or Xiegu code.

Goal: replace the R1CBU control layer and the patched baseband dependency with an
independent, documented library, as the base for a new front end.

## Status

Verified on hardware (BASE 1.1.9), see `docs/x6100_control_interface.md` §9:

- status block, calibration read with CRC check
- flow frames (IQ + telemetry), CRC checked
- register file, full and diff writes
- RX tuning: frequency, mode, filter, band, volume, per-band calibration

Not yet: TX, audio (ALSA), front panel input, display, GPIO.

## Layout

| Path | Content |
|------|---------|
| `include/x6100.h` | public API |
| `include/x6100_regs.h` | register map, packing macros |
| `include/x6100_cal.h` | calibration block layout (0xF000) |
| `src/x6100.c` | implementation |
| `tools/x6100_info.c` | read-only check: version, calibration, flow |
| `tools/x6100_rodump.c` | raw dump of status and calibration blocks |
| `tools/x6100_tune.c` | RX-only tuning test, TX blocked |
| `scripts/x6100_ui.sh` | stop/start the GUI (stock and R1CBU images) |
| `docs/x6100_control_interface.md` | protocol documentation |

## Build

Cross-compile on openSUSE Tumbleweed:

```
zypper install zig
bash build.sh
```

Output in `out/`: `libx6100.a` and static ARM tools.

## Run on the radio

```
scripts/x6100_ui.sh stop
./x6100_info
./x6100_tune 14074000 usb 30 20
scripts/x6100_ui.sh start
```

The GUI must be stopped: it writes the same bus every millisecond.

## Safety

- No function writes the calibration block; keep your own dump as backup.
- The tools never set PTT. Test any future TX code into a dummy load.

## License

GPL-3.0-or-later, see `LICENSE`.
