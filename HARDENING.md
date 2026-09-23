# Bounded serial transfers

This branch retains the command-line and statistics format of source revision
8da2a47b58e14326c8eb1fe6bdeffbc5b908f8a5, with stricter failure handling.

- Reads and writes process one available chunk per event. Partial responses and
  EAGAIN return to polling; deadlines and SIGTERM remain effective. The legacy
  `--rx-bytes-threash` / `-M` option remains accepted as a compatibility hint; it
  never requires a reader to wait for a complete block. `--initiate-tx` / `-F`
  maintains one block of initial write-follow credit, including short writes.
- Duration limits use monotonic milliseconds. A signal, disconnected device,
  permanent I/O error, or new driver error fails the test. Output is line buffered;
  statistics remain available on graceful termination. Detailed mismatch logs are
  capped at 32; all detected mismatches are counted.
- Polling requests writable events only with available transmission credit and
  after configured delays. Inactivity timestamps advance only on actual I/O.
- `--rs485 AFTER[.BEFORE]` uses integer **milliseconds**, matching the Linux API.
  Explicit requests are applied even when RS485 is already enabled, verified by
  readback, and restored on exit. Without this option the existing RS485 mode is
  preserved. Local RX during TX is not requested, to avoid mistaking echo for a
  peer response. Unsupported explicit configuration fails.
- `--turnaround-delay MS` delays a reply after the latest receive activity.
  Choose it to cover the peer's driver-release time. No generic value can prove
  electrical bus turnaround; validate the physical transceiver separately.
- Setup saves and verifies terminal settings. Cleanup restores terminal, changed
  custom divisor, RS485, and requested loopback state. A failed advisory lock does
  not flush or reconfigure the port. Other applications that ignore advisory locks
  still require external coordination.
- Flushing occurs only with `--flush-buffers`, with no deferred 100 ms discard.
  The `Ready:` line is emitted after configuration/flush. Start the receiver before
  the transmitter, allow a receive tail, and do not flush a live exchange.
- `--drain-timeout MS` bounds output-queue draining (default 10000). UARTs exposing
  transmitter-empty state are checked too. Some USB drivers cannot expose bytes
  still inside their adapter: drained/accepted TX counts are not proof of delivery.
  There is no unbounded tcdrain or destructive exit flush, including single-byte mode.
- Empty RX fails unless explicitly allowed by `--expected-rx 0`.
  `--expected-rx N` requires exactly N received bytes and detects missing entire
  periods of the pattern when the sender's intended count is independently known.

## Integrity limits

The legacy pattern repeats every 256 bytes. `rx err` counts **sequence
mismatches**, not physical corrupted or missing bytes. Resynchronization counts
one discontinuity even if several bytes are missing. No receiver can distinguish
loss of a whole pattern period from this stream alone. Use an independently known
`--expected-rx` value for count-sensitive transfers. Sequence-numbered frames and
CRC would be a separate, incompatible protocol extension, not a claim made by
this utility. A minimum throughput assertion alone cannot detect all loss.

Driver counters retain their existing cumulative statistics line for parsers;
a separate error-delta line reports only increments during this invocation and
causes failure on new overruns, framing/parity/break or buffer-overrun errors.
A status of 125 indicates a fatal transfer, driver, expected-count, or empty-RX
failure; lower nonzero statuses retain legacy mismatch/count semantics.

## Validation and building

```
gcc -std=gnu11 -O2 -Wall -Wextra -Werror linux-serial-test.c -o linux-serial-test
python3 -m unittest discover -s tests -v
```

Tests cover PTY transfer/termination/locking/restore and mocked driver contracts.
They do not validate electrical RS485 behavior. `tests/build.Dockerfile` defines
an x86-64/ARM hard-float cross-build environment; `tests/build.sh` builds static
binaries embedding the source commit in `--version`. Record compiler/package
versions and binary SHA256 values when distributing them. Run builds from a clean
committed checkout so the embedded revision accurately identifies the source.
