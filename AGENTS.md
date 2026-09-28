# TAEK OS Agent Guide

## Project shape

TAEK OS is an experimental freestanding x86_64 UEFI kernel in C11 and NASM. The entry point is `nucleo/principal.c`; Limine supplies boot services such as the framebuffer, HHDM, memory map, executable cmdline, and modules. The kernel runs in Ring 0 and talks directly to PCI, MMIO, DMA, xHCI, HDA/AC97, and USB storage.

Keep user-facing and source naming consistent with the existing Spanish vocabulary (`nucleo`, `base`, `controladores`, `memoria`, `paginacion`, `principal`). Do not rename public APIs or directories casually.

## Build and test

- On Windows, the normal end-to-end command is `./run.ps1` from PowerShell. It invokes WSL for `make`, then launches QEMU with OVMF, xHCI, USB keyboard/storage, and selected audio.
- In WSL, the kernel build is `make`. `make all` builds both `build/taek-os.img` and `build/taek-os.iso`; `make clean` removes generated build output after preserving dated ISOs in `build antigua/`.
- Useful QEMU targets are `make qemu` and `make qemu-trace`.
- Required build tools include Clang, LLD, NASM, GNU Make, FFmpeg, mtools, xorriso, WSL, QEMU, and OVMF. The Makefile uses POSIX shell commands, so run it in WSL rather than native PowerShell.
- Host filesystem checks are `bash tests/probar_fat32_host.sh` and `bash tests/probar_exfat_host.sh`. They build sanitizer-instrumented host programs and validate the resulting images with filesystem checkers.
- Treat QEMU success and host tests as separate evidence. Hardware-only behavior must be validated on the documented physical profiles and recorded in `BITACORA.md`.

## Source ownership

- `nucleo/principal.c`: boot order, Limine cmdline mode selection, subsystem sequencing, splash/audio startup, and terminal handoff.
- `nucleo/arquitectura/x86_64/`: CPU tables and traps, serial, ports, PCI, APIC, VMX, and low-level assembly.
- `nucleo/base/`: PMM and heap, paging/VMM, DMA arena/coherency, timing, UTF-8, versioning, and the Stability Egg integrity checks.
- `nucleo/controladores/xhci.c`: xHCI rings, slots, endpoint contexts, HID, hotplug, and USB MSC endpoint plumbing. Respect xHCI reserved bits, cycle bits, event-ring ownership, DMA cache barriers, and hardware timeouts.
- `nucleo/controladores/audio_hda.c` and `audio_ac97.c`: hardware audio selection and PCM streaming. Keep HDA state transitions, DMA position tracking, prebuffering, backpressure, and stop/reset handshakes explicit.
- `nucleo/controladores/{usb_msc,vfs,fat32,exfat,ntfs,ext4}.*`: USB BOT/SCSI and filesystem mounting, cataloging, reads, and any supported writes. Validate sector geometry, partition bounds, cluster/run lengths, record sizes, and I/O results before touching buffers.
- `nucleo/controladores/multimedia/`: MP4 demux, H.264, AAC, image conversion, and playback. Preserve the freestanding/no-libc contract and the documented codec limits.
- `nucleo/compatibilidad/` and `nucleo/controladores/video/nvidia/`: compatibility shims and experimental NVIDIA inventory/interface code. Do not claim GSP firmware loading, RPC, VM exits, or GPU operational state unless real hardware and signed firmware have actually confirmed it.

## Hardware and safety rules

- Never run QEMU against an image while WSL is still formatting or copying it. Create raw test images under WSL `/tmp`, run `sync`, then copy them to `build/` before QEMU opens them. This avoids Windows/WSL image contention; see [ESTADO_SUBSISTEMAS.md](ESTADO_SUBSISTEMAS.md).
- Do not flash or write a physical disk unless the user explicitly identifies the target. Prefer `run.ps1` virtual disks and read-only validation.
- Avoid destructive kernel tests (`rm -rf` tiers, triple-fault paths, raw hardware resets) during ordinary validation. VMXON alone is preliminary: there is no configured guest, VM-exit handler, or EPT.
- Treat generated files under `build/`, `build antigua/`, and QEMU logs as artifacts, not source. Do not edit them to fix a source problem.
- ISO names must use exactly `taek-os-YYYY-MM-DD_HH-MM-SS.iso`, with no additional prefixes or suffixes. Keep the latest ISO directly in the root of `build/`, never in a subdirectory. Archive superseded ISOs in `build antigua+` without overwriting existing archives or moving an ISO used by QEMU.
- Preserve existing user changes and unrelated worktree changes. Do not commit or push changes unless requested.

## Documentation and change discipline

## Documentation and change discipline

Read the relevant focused document before changing a subsystem:

- [ESTADO_SUBSISTEMAS.md](ESTADO_SUBSISTEMAS.md): current status, validation levels, and known gaps.
- [GUIA_HARDWARE_REAL.md](GUIA_HARDWARE_REAL.md): physical boot and diagnostic procedure.
- [PERFILES_HARDWARE.md](PERFILES_HARDWARE.md): known Intel hardware profiles.
- [H264_VALIDACION.md](H264_VALIDACION.md): codec coverage, reference comparisons, and limits.
- [PLAN_H264_RENDIMIENTO.md](PLAN_H264_RENDIMIENTO.md): multimedia measurement contract and optimization work.
- [PLAN_AUDIO_HDA_AV.md](PLAN_AUDIO_HDA_AV.md): HDA/audio-video streaming plan and measurements.
- [BITACORA.md](BITACORA.md): chronological decisions, forensic failures, fixes, and hardware validation evidence.

### Mandatory BITACORA.md Structure (Non-Negotiable)

DO NOT write generic release notes, high-level bullet summaries, or brief change lists. Every entry in `BITACORA.md` must follow the exact forensic depth of **Hito 65**. 

Every new milestone must strictly include these 5 sections in order:

1. **Objetivo y Contexto:**
   Precise technical goals, target hardware/codecs, and performance invariants (e.g., zero underruns, monotonic master clock, memory caps).
2. **Causas Raíz Identificadas e Inspección Forense de Código:**
   Deep forensic post-mortem of why previous or existing logic fails. Must document exact function names, source lines, struct fields, bit widths, and arithmetic mismatches (e.g., packet size vs. DMA ping-pong buffers, time/byte granularities, lock-step starvation, register desync). If mid-implementation blockers or encoding/build corruptions occurred, document them here in detail.
3. **Soluciones de Ingeniería Implementadas:**
   Architectural and structural fixes. Detail state machines, buffer handshakes, memory boundaries (DMA/heap arenas), algorithm changes (e.g., contiguous block copy vs. scalar loops), and hardware register synchronization mechanisms.
4. **Pruebas y Verificación Forense (Datos Duros):**
   A successful `make` build is NOT proof of completion. Log concrete telemetry:
   - For audio/video: exact sample counts, analyzed time windows (e.g., 100 ms blocks), dropped frames, underrun counts, continuous audio duration, and waveform continuity.
   - For hardware/DMA: MMIO register progression (LPIB/DPIB, BCIS event counts), ring transitions, and DMA block rotation logs.
   - If tested strictly in bare metal or QEMU raw capture, state the exact environment and output metrics. Never close an entry with just "compiled successfully".
5. **Archivos Modificados y Límites Conocidos:**
   Explicit file list and strict technical boundaries remaining (supported formats, unhandled edge cases, pending manual hardware validation).

## Validation expectations

- A clean compilation or linker pass (`make`) is merely step zero; it never constitutes validation evidence on its own.
- For host-side logic, run `bash tests/probar_fat32_host.sh` and `bash tests/probar_exfat_host.sh`.
- For kernel execution, validation is judged against real silicon hardware invariants (strict FPU/SSE context switching, physical DMA rings, hardware audio codecs, and unbuffered USB latencies).
- When physical bare-metal testing is required by the user, report all code as *pending physical validation* until verified on actual silicon, capturing register state and telemetry rather than assuming emulator behavior.
