#!/bin/sh
# Superseded by the Rust integration test tests/integration/tests/native_qemu.rs.
# The Rust test builds and runs the gate test harness inside QEMU directly.
echo "Use: cargo test -- native_riscv_qemu_gate --nocapture" >&2
exit 1
