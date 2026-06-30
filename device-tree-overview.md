# Device Trees Across UEFI, GRUB2, U-Boot and the Linux Kernel

This document describes, for each of the four projects, where the device tree
lives, how it is compiled/integrated into the build, how it is loaded at run
time, and how it can be replaced with a different one. The four projects sit at
different points on the boot chain, so they treat the device tree in quite
different ways:

```
  Power on
     │
     ▼
  [ UEFI firmware ]   ── owns/provides a DTB, installs it in the EFI
     │                    Configuration Table (gFdtTableGuid)
     ▼
  [ GRUB2 ]           ── optionally loads a *new* DTB and re-installs it
     │                    into the EFI Configuration Table
     ▼
  [ U-Boot ]          ── uses its own control DTB; also prepares and
     │                    passes a DTB to the OS (and runs fixups)
     ▼
  [ Linux kernel ]    ── receives a DTB pointer, unflattens it, builds the
                          live device model from it
```

Note that in a real system you typically use *one* of UEFI/U-Boot as the
primary firmware, optionally with GRUB2 in between, before the Linux kernel.
They are described together here for comparison.

---

## 0. Device Tree fundamentals (shared by all four)

Before the project-specific detail, the vocabulary is common to all of them:

| Term     | Meaning |
|----------|---------|
| `.dts`   | Device Tree **Source** — human-readable description of one board/SoC. |
| `.dtsi`  | Device Tree Source **Include** — shared fragments (`#include`d into `.dts`), e.g. an SoC base file included by many board files. |
| `.dtb`   | Device Tree **Blob** — the compiled binary, also called the **FDT** (Flattened Device Tree). This is what software actually consumes at run time. |
| `.dtbo`  | Device Tree **Overlay** blob — a compiled overlay fragment that patches a base `.dtb`. |
| `dtc`    | The **Device Tree Compiler**: `dts → dtb` (and back, and overlays). |
| FDT      | The on-disk/in-memory binary format (magic `0xd00dfeed`, big-endian). The `libfdt` C library reads and modifies it in place. |

The DTB is self-describing, position-independent, and big-endian regardless of
the CPU endianness. Every project in this document ultimately deals in the same
FDT binary; they differ in *where it comes from* and *who hands it to whom*.

A typical modern source file is preprocessed by `cpp` (so `#include`,
`#define`, macros from `dt-bindings/` headers all work) and then fed to `dtc`.

---

## 1. UEFI (EDK2 / TianoCore)

On most ARM/AArch64 (and RISC-V) UEFI platforms, the firmware is responsible for
either presenting hardware via **ACPI** *or* via a **Device Tree**, and for
publishing that DTB to later boot stages.

### Where it is stored

There are several common sources, and a given platform usually supports one or
two of them:

1. **Embedded in a Firmware Volume (FV) inside the firmware image.**
   The `.dtb` is built into the flash image as a raw-section FFS file. EDK2's
   `EmbeddedPkg/Drivers/DtPlatformDxe` together with a board-supplied
   `DtPlatformDtbLoaderLib` reads this embedded copy. In the EDK2 source tree
   the device tree sources for a platform typically live under the platform
   package, e.g.
   `Platform/<Vendor>/<Board>/.../DeviceTree/*.dts` or are pulled in from an
   external `.dtb` referenced by the platform `.fdf`/`.dsc`.

2. **Passed in from a prior boot stage.**
   On ARM, TF-A (Trusted Firmware-A / BL31) or an earlier loader may place a
   DTB in DRAM and pass its address to UEFI. UEFI then adopts that blob.

3. **Loaded from a file system** (e.g. a `.dtb` on the EFI System Partition),
   when the platform's `DtPlatformDtbLoaderLib` is implemented that way.

### How it is compiled / integrated

- The `.dts`/`.dtsi` sources are compiled with `dtc` (sometimes via the
  Linux kernel's copy of `dtc`) into a `.dtb`.
- That `.dtb` is referenced from the platform **`.fdf`** (Flash Description
  File) so the build tools place it into a Firmware Volume as a FFS section, and
  from the **`.dsc`** so the relevant DXE driver and `DtPlatformDtbLoaderLib`
  are included.
- The EDK2 `build` command assembles everything into the final flash image
  (`*.fd`).

### How it is loaded / published

- `DtPlatformDxe` (a DXE driver) runs during the DXE phase. On platforms that
  support both ACPI and DT it consults a setup variable (`DtAcpiPref`) to decide
  which to expose; if DT is chosen it removes the ACPI tables and installs the
  DTB.
- The DTB is installed into the **UEFI Configuration Table** under the GUID
  `gFdtTableGuid` (`b1b621d5-f19c-41a5-830b-d9152c69aae0`,
  `EFI_DTB_TABLE_GUID`). Anything later in the chain (GRUB, systemd-boot, the
  Linux EFI stub, U-Boot's EFI payload mode) can find the DTB by scanning the
  configuration table for this GUID.
- Before installing it, UEFI may apply fixups (memory node, `/chosen`, reserved
  regions, PSCI, etc.) so the published DTB matches the actual platform.

### How to replace it

- **Rebuild the firmware** with a new embedded `.dtb` (replace the source/blob
  referenced by the `.fdf`, re-run the EDK2 build, reflash).
- **Provide the DTB from the ESP**: on platforms whose `DtPlatformDtbLoaderLib`
  reads a file, drop a replacement `*.dtb` on the EFI System Partition.
- **Override it later in the chain**: because the kernel ultimately uses whatever
  DTB is in the configuration table at hand-off, a later loader (GRUB's
  `devicetree` command, systemd-boot's `devicetree` stanza, or U-Boot) can
  install a different blob and override the firmware's.
- On platforms that take the DTB from a prior stage, replace the blob that
  TF-A / the earlier loader hands up.

---

## 2. GRUB2

GRUB2 is (mostly) a *pass-through* with respect to device trees: it does not
maintain an internal DTB to describe its own hardware (it relies on the firmware
— BIOS/UEFI/coreboot — or on platform-specific code). What it *does* provide is
the ability to **load a DTB from a file and hand it to the OS**, which is very
commonly used on ARM/AArch64 EFI systems.

### Where it is stored

- The DTB GRUB loads is an ordinary file on a filesystem GRUB can read
  (e.g. `/boot/dtb/<vendor>/<board>.dtb`, the ESP, `/boot`, etc.).
- The relevant functionality is the **`devicetree`** command, provided by the
  `fdt` module (`grub-core/loader/efi/fdt.c` and the arch loaders). On EFI
  platforms the module is `fdt.mod`; it must be present in the GRUB image /
  loadable from the module directory.

### How it is integrated

- GRUB itself is built with its normal toolchain; the `fdt`/loader modules are
  compiled in or loaded as `.mod` files. There is no `dtc` step in GRUB — it
  consumes an already-compiled `.dtb`.
- Usage is configured in **`grub.cfg`** (or interactively at the GRUB prompt):

  ```
  devicetree /boot/dtb/rk3399/rock-pi-4.dtb
  linux      /boot/vmlinuz root=/dev/...
  initrd     /boot/initrd.img
  boot
  ```

### How it is loaded / handed off

- The `devicetree <file>` command reads the blob into memory and, on EFI,
  **installs it into the EFI Configuration Table under `gFdtTableGuid`**,
  replacing any DTB the firmware (Section 1) had published.
- When the subsequent `linux` command boots the kernel via the EFI stub, the
  kernel finds this DTB through the configuration table.
- On non-EFI ARM/U-Boot-style targets, GRUB's loader passes the loaded FDT
  address to the kernel through the architecture's normal boot register
  convention.
- Issuing `devicetree` with no argument (or unloading) drops the GRUB-supplied
  DTB so the firmware's original one is used again.

### How to replace it

- Point the `devicetree` line in `grub.cfg` at a different `.dtb`, or type a new
  `devicetree` command at the GRUB shell, then re-boot the kernel.
- Drop a new `.dtb` into the directory referenced by `grub.cfg`.
- Remove/omit the `devicetree` line entirely to fall back to the firmware's DTB.
- (GRUB has no overlay-apply command; overlays must be pre-merged into the base
  DTB beforehand, e.g. with `fdtoverlay`.)

---

## 3. U-Boot

U-Boot is special because it uses a device tree **twice**:

1. **Its own "control" DTB** — U-Boot's driver model (DM) and most of its
   drivers are configured from a device tree (`CONFIG_OF_CONTROL`). This is how
   U-Boot knows about its own UART, MMC, clocks, etc.
2. **The DTB it passes to the OS** — when booting Linux, U-Boot loads (or
   reuses) a DTB, applies run-time fixups, and hands it to the kernel.

### Where it is stored

**Control DTB sources** live in-tree at `arch/<arch>/dts/<board>.dts` (plus
`.dtsi` files), and increasingly the very same SoC/board `.dts` files are shared
with / synced from the Linux kernel. The compiled control DTB ends up combined
with the U-Boot binary in one of several ways, selected by Kconfig:

| Kconfig                | Where the control DTB lives |
|------------------------|-----------------------------|
| `CONFIG_OF_SEPARATE`   | Built separately and **appended** to the U-Boot binary → `u-boot-dtb.bin` / `u-boot.dtb`. The most common production setup. |
| `CONFIG_OF_EMBED`      | Embedded into the U-Boot ELF (in a data section). Convenient for debugging, discouraged for production. |
| `CONFIG_OF_BOARD`      | Provided at run time by a board-specific function (`board_fdt_blob_setup()`), e.g. read from a prior stage or hardware. |
| `CONFIG_OF_PRIOR_STAGE`| The DTB is passed to U-Boot in a register by an earlier boot stage (e.g. on RISC-V, by OpenSBI/the SPL). |

The **OS DTB** (the one passed to Linux) is usually a separate file/artifact:
on a boot partition (`/boot/<board>.dtb`), inside a **FIT image** (see below),
or — on simple setups — reused directly from U-Boot's own control DTB.

### How it is compiled / integrated

- U-Boot's build (Kbuild, like the kernel) preprocesses the `.dts` with `cpp`
  and compiles it with the in-tree `dtc` (`scripts/dtc`), producing the control
  `.dtb`, which is then appended/embedded per the table above.
- For delivering the OS DTB and kernel together, U-Boot's `mkimage` tool builds
  a **FIT image** (`.itb`, Flattened Image Tree) from an **`.its`** source. A
  FIT can bundle the kernel, one or many DTBs, ramdisks, and named
  `configurations` that select which DTB goes with the kernel — including
  signatures for verified boot.

### How it is loaded / handed off

- At start-up U-Boot locates its control DTB (appended blob / embedded / prior
  stage) and uses it to probe its own drivers.
- To boot Linux, U-Boot loads the kernel and a DTB into RAM and runs one of:
  - `bootm` (uImage/FIT), `booti` (arm64 `Image`), `bootz` (arm `zImage`),
    `bootefi` (EFI payload).
  The DTB address is given as the last argument, e.g. `booti $kernel_addr - $fdt_addr`
  (the `-` means "no ramdisk").
- Before jumping to the kernel, U-Boot performs **fixups** on the DTB: it fills
  in `/chosen` (`bootargs`, `linux,initrd-start/end`, `kaslr-seed`,
  `stdout-path`), the `/memory` node (actual RAM size/banks), MAC addresses,
  reserved-memory, serial-number, etc. Board code hooks this via
  `ft_board_setup()` / `ft_system_setup()`.
- It then passes control with the DTB pointer in the architecture register
  (arm64: `x0`; arm: `r2`), exactly as the kernel expects.

### Run-time inspection / manipulation

U-Boot has a rich **`fdt`** command for working on the DTB in place before
boot:

```
fdt addr ${fdt_addr}          # tell U-Boot which blob to operate on
fdt print /soc                # dump a node
fdt set /chosen bootargs "console=ttyS0,115200 root=/dev/mmcblk0p2"
fdt mknode / newnode
fdt apply ${overlay_addr}     # apply a .dtbo overlay onto the base DTB
```

This makes U-Boot the most flexible stage for live device-tree editing.

### How to replace it

- **OS DTB (most common):** change the `fdtfile`/`fdt_addr` environment
  variables, or drop a different `.dtb` into the boot partition, so a different
  blob is loaded before `booti`/`bootm`. Distro-boot scripts pick the DTB by the
  `fdtfile` variable automatically.
- **Apply overlays:** load `.dtbo` files and run `fdt apply` (or use a FIT
  configuration that references overlays) to patch the base DTB without
  replacing it wholesale.
- **FIT image:** rebuild the `.itb` (new `.its` referencing new DTBs), or select
  a different `configuration` within the FIT.
- **Control DTB (U-Boot's own):** rebuild U-Boot after editing
  `arch/<arch>/dts/<board>.dts` (regenerates the appended/embedded blob), or,
  for `CONFIG_OF_BOARD`/`OF_PRIOR_STAGE`, change what the earlier stage supplies.

---

## 4. Linux kernel

The kernel is the ultimate consumer: it does not normally produce the DTB it
runs on (the bootloader hands that in), but it *can* build DTBs, and on some
configurations it carries one itself.

### Where it is stored

- **Source** lives in the kernel tree under
  `arch/<arch>/boot/dts/`:
  - `arch/arm/boot/dts/<board>.dts`
  - `arch/arm64/boot/dts/<vendor>/<board>.dts` (vendor sub-directories)
  - `arch/riscv/boot/dts/<vendor>/...`, `arch/powerpc/boot/dts/...`, etc.
  - Shared SoC fragments are `.dtsi` files included by the board `.dts`.
  - Bindings/constants headers live under `include/dt-bindings/`.
- **Compiled output** (`.dtb`, and `.dtbo` for overlays) is produced *next to*
  the sources in the same `arch/.../boot/dts/` tree during the build.

### How it is compiled / integrated

- The kernel build system (Kbuild) preprocesses each `.dts` with `cpp` and
  compiles with the in-tree `dtc` (`scripts/dtc`).
- Targets:
  - `make dtbs` — build all DTBs selected by the current config
    (`CONFIG_OF`, and the per-board `dtb-$(CONFIG_...)` lists in each
    directory's `Makefile`).
  - `make dtbs_install` — install them (to
    `/boot/dtbs/<kernelrelease>/...` or `$INSTALL_DTBS_PATH`).
  - `make dtbs_check` / `CHECK_DTBS=1` — validate against YAML bindings
    (`Documentation/devicetree/bindings/`, using `dt-schema`).
- The DTBs are normally **separate artifacts** shipped alongside the kernel
  image. Two integration variants exist on ARM:
  - **`CONFIG_ARM_APPENDED_DTB`** — the bootloader-agnostic trick of
    concatenating a `.dtb` to the end of the `zImage` (`cat zImage board.dtb >
    zImage-dtb`) for old bootloaders that cannot pass a DTB pointer.
  - **`CONFIG_ARM_ATAG_DTB_COMPAT`** — let the kernel accept legacy ATAGs and
    convert/merge them into the appended DTB.

### How it is loaded

- The standard hand-off: the bootloader places the DTB in RAM and passes its
  physical address to the kernel in the agreed register —
  **arm64: `x0`**, **arm: `r2`**, with matching conventions on other arches
  (RISC-V: `a1`).
- Early boot (`setup_arch()` → `early_init_dt_scan()`) verifies the FDT magic,
  reads `/chosen` (kernel command line, initrd), `/memory`, and reserved
  regions.
- The kernel then **unflattens** the FDT into the live, in-memory tree of
  `struct device_node`, which the driver core and `of_*` APIs use to match and
  probe drivers.
- In the **EFI boot** path, the EFI stub instead finds the DTB via the EFI
  Configuration Table (`gFdtTableGuid`) that UEFI/GRUB installed (Sections 1–2),
  unless ACPI is being used.

### Overlays at run time

- With `CONFIG_OF_OVERLAY`, compiled overlays (`.dtbo`) can be applied to the
  live tree after boot — via the bootloader (U-Boot `fdt apply`), via a
  platform/firmware mechanism, or through `configfs`
  (`/sys/kernel/config/device-tree/overlays/`). This adds/modifies nodes (e.g.
  to enable a HAT/cape/expansion device) without rebuilding the base DTB.

### How to replace it

- **Swap the file the bootloader loads.** The cleanest method: build a new
  `.dtb` (`make dtbs`), install it to `/boot/...`, and point the bootloader at
  it (GRUB `devicetree`, U-Boot `fdtfile`, systemd-boot `devicetree`, the EFI
  config table, etc.). The kernel binary itself need not change.
- **Edit and rebuild from source.** Modify the `.dts`/`.dtsi` under
  `arch/<arch>/boot/dts/`, `make dtbs`, redeploy.
- **Apply an overlay** instead of replacing the whole tree (`CONFIG_OF_OVERLAY`
  + `.dtbo`), for incremental hardware changes.
- **Appended DTB.** For `CONFIG_ARM_APPENDED_DTB`, rebuild the
  `zImage`+`.dtb` concatenation with the new blob.
- **Decompile/recompile in place.** Since DTBs are self-describing, an existing
  `.dtb` can be turned back to source with `dtc -I dtb -O dts board.dtb` ,
  edited, and recompiled with `dtc -I dts -O dtb`, without the kernel tree at
  all — handy for quick field fixes.

---

## 5. Side-by-side summary

| Aspect | UEFI (EDK2) | GRUB2 | U-Boot | Linux kernel |
|--------|-------------|-------|--------|--------------|
| **Source location** | Platform pkg `*.dts`, or external blob referenced by `.fdf` | n/a (consumes a prebuilt `.dtb`) | `arch/<arch>/dts/*.dts` (control DTB); OS DTB as separate artifact/FIT | `arch/<arch>/boot/dts/*.dts(i)` |
| **Compiled with** | `dtc`, packed into FV by EDK2 `build` | not compiled by GRUB | `dtc` (Kbuild) + `mkimage` for FIT | `dtc` (Kbuild), `make dtbs` |
| **Where the blob lives** | Embedded in firmware FV / from prior stage / ESP file | File on a GRUB-readable FS | Appended to `u-boot.bin` / embedded / FIT / boot partition / prior stage | Separate `.dtb` (usually); optionally appended to `zImage` |
| **How handed on** | Installed in EFI Config Table (`gFdtTableGuid`) | `devicetree` cmd → EFI Config Table (or boot reg) | Reg (`x0`/`r2`) via `booti`/`bootm`; after fixups | Receives reg pointer / EFI config table; unflattens |
| **Live editing** | Build-time fixups in DXE | none (pre-merge overlays) | rich `fdt` cmd, `fdt apply` overlays, board fixups | `CONFIG_OF_OVERLAY` via configfs |
| **Replace by** | Rebuild firmware / ESP file / override downstream | edit `grub.cfg` `devicetree` line | change `fdtfile`/FIT/overlay; rebuild control DTB | swap loaded `.dtb`, overlay, or rebuild `make dtbs` |

### Key cross-cutting points

- **One binary format, many couriers.** All four exchange the same FDT blob; the
  differences are about *storage* and *who installs it where*.
- **The EFI Configuration Table (`gFdtTableGuid`) is the common rendezvous** for
  the UEFI → GRUB → kernel path: each stage can override the previous stage's
  DTB simply by re-installing one under that GUID.
- **U-Boot is the most DT-centric loader:** it both *consumes* a DT for itself
  and *prepares* one (with fixups and optional overlays) for the OS.
- **The kernel prefers an externally supplied DTB**, which is exactly why
  replacing a board's device tree is usually a bootloader-config change rather
  than a kernel rebuild.
