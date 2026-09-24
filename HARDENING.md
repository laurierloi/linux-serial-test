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

## Opt-in framed half-duplex transactions

Start a responder first and wait for its `Ready:` line or exclusive `--ready-file`
marker, then start the initiator with the same nonzero `--run-id`, `--transactions`
and `--payload-bytes`. For example, on two connected endpoints:

```
linux-serial-test -p /dev/ttyUSB0 -b 9600 --transaction-role responder --run-id 123 --transactions 16 --payload-bytes 64 --startup-timeout 10000 --transaction-timeout 3000 --turnaround-delay 2
linux-serial-test -p /dev/ttyS3 -b 9600 --transaction-role initiator --run-id 123 --transactions 16 --payload-bytes 64 --startup-timeout 10000 --transaction-timeout 3000 --turnaround-delay 2
```

Both endpoints negotiate the count and payload size before data, then alternate
complete requests and responses. A final exchange reconciles completion. There
are no automatic retries: failures remain visible. `--startup-timeout` bounds
initial negotiation separately (otherwise it uses the transaction deadline).
Each following exchange shares one deadline across send, drain, receive and guard.
Choose a deadline longer than both frames' wire time plus guard/adapter delays.

The wire format is `LST1`, type (1 byte), sender role (1), payload length (2),
run ID (4), sequence (4), payload (0..4096), and IEEE CRC32 (4). Multibyte values
use network byte order; CRC covers header and payload. Types are HELLO=1,
READY=2, REQUEST=3, RESPONSE=4, DONE=5, ACK=6. Roles are initiator=0 and responder=1.
Control payloads contain count and size (two 32-bit integers). Sequences start at1
for data and use count+1 for completion. Payloads use a reproducible run/sequence
seed and responses invert each generated byte with0xa5, preventing a simple echo
from passing as a response. This is a diagnostic protocol, not authentication.
Malformed frames fail immediately instead of trying to resynchronize or retry.

A `Transaction result:` JSON line reports role/run ID, completed exchanges,
validated payload byte counts in each direction, first failure category, drain
assurance, and mean/max exchange time in milliseconds. The responder timing
includes waiting for the next request; it is not a one-way propagation metric.
CRC, truncation, timeout, wrong session, sequence, local echo, configuration,
I/O, drain, driver-counter and cleanup failures are distinguishable. Both endpoint
reports and zero exit statuses must be checked; a responder cannot infer that its
last acknowledgment reached a peer that disappeared.

The receive guard starts only after a complete validated frame. It is a scheduler
based minimum wait, not a real-time DE waveform guarantee. `--strict-drain`
requires driver-provided physical transmitter-empty feedback and fails on drivers
without it. Otherwise JSON explicitly reports `driver_queue_only` when appropriate.
Even transmitter-empty feedback does not independently prove physical DE release.
Termios/RS485 restoration errors now affect normal exit status; cleanup is idempotent.

`--ready-file` exclusively creates a marker after configuration and any explicit
flush, and removes its own marker on normal/graceful exit. It never replaces an
existing file. SIGKILL cannot clean it; use a fresh per-run path and remove stale
markers only when their owning test is known to have ended. Framed mode cannot be
combined with stream duration/direction, write-follow, loopback or byte-count flags.

## Versioned CI releases

`VERSION` holds the semantic version. Builds report `v<VERSION>+<source SHA>`.
Push the matching `v<VERSION>` tag to publish after all regression checks pass.
A suffix such as `-rc.1` produces a prerelease, never the latest stable release.
CI builds static x86-64 and ARM hard-float executables inside the recorded Docker
image and runs the PTY suite on both (ARM through QEMU). Mock driver tests run
natively. CI does not qualify electrical behavior on hardware.

Release assets are `linux-serial-test-linux-x86_64`,
`linux-serial-test-linux-armhf`, `manifest.json`, and `SHA256SUMS`. The manifest
records the exact commit, compiler versions, build image ID, flags and hashes.
The repository must have immutable releases enabled; publication fails otherwise.
GitHub creates release attestations when the draft is published. Consumers can
verify with `gh release verify <tag>` and `gh release verify-asset <tag> <file>`.
Pin a release and its binary SHA-256 values in consuming projects, never `latest`.
To roll back, restore the previous lockfile and reinstall its pinned binaries.

Change VERSION in a reviewed commit, then tag that commit. Published versions
and their assets are not overwritten. A failed draft publication can be inspected
and completed after fixing the failure; the workflow intentionally does not
replace existing assets on a rerun.
