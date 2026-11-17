# Windows host file buffering

Compile with `cmake -S tests/kernel_file_buffers -B build-file-buffers -A x64`,
build Release or Debug, then run CTest for that configuration.

The fixture compiles the complete production Windows file backend. Only path
translation and logging are stubbed. A host-open observer records the flags
before forwarding to the real Windows API; data reads, events and EOF use real
temporary files. It checks both cached and Xbox uncached opens, BLACK's reported
231424-byte request for a 229664-byte file, a 2048-byte-aligned buffer and offset
that do not meet 4096-byte alignment, short reads, event signaling, byte-level
host alignment independence and EOF. The test needs no game files or 4Kn drive.

Restoring the old `FILE_FLAG_NO_BUFFERING` mapping must fail the host-flags
assertion even on a 512-byte-sector drive. This proves the host requirement is
removed, not a physical 4Kn gameplay test.
Use `-DKERNEL_FILE_TEST_SOURCE=<absolute old kernel_file.c>` in a separate build
to run that negative control.
