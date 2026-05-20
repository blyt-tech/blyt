#!/bin/sh
# Superseded by the Rust integration test tests/integration/tests/native_qemu.rs.
# Run the QEMU gate test with:
#   BLYT_QEMU_KERNEL=... BLYT_QEMU_ROOTFS=... cargo test -- native_riscv_qemu_gate --nocapture
echo "Use: cargo test -- native_riscv_qemu_gate --nocapture" >&2
exit 1
