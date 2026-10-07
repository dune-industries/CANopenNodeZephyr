# CANopenNode v4.x Protocol Stack for Zephyr RTOS

## What is this

[CANopenNode](https://github.com/CANopenNode/CANopenNode) is a free and open source CANopen
protocol stack written in C. This repo, CANopenNodeZephyr, is the glue layer that connects it
to the [Zephyr RTOS](https://github.com/zephyrproject-rtos/zephyr). It has the Zephyr CAN driver,
Kconfig options, CMake build files, and a working sample.

This code has been tested on STM32F072 (bxCAN), STM32H7(M_CAN), and FRDM-MCXN947 (FlexCAN) series. 

### Files

| Path | What it does |
|---|---|
| `CANopenNode/` | Upstream CANopenNode v4.x (git submodule) |
| `src/CO_driver.c` | Zephyr CAN driver glue: TX workqueue, RX filters, mutexes |
| `src/canopen_leds.c` | CiA 303-3 LED indicators over GPIO |
| `src/canopen_sync.c` | Separate SYNC producer/consumer thread |
| `src/canopen_program.c` | CiA 302-3 program download (firmware update over CAN with MCUboot) |
| `include/canopennode.h` | Public glue API (LEDs, program download) |
| `include/CO_driver_target.h` | Zephyr-specific macros |
| `Kconfig` | All `CONFIG_CANOPENNODE_*` options |
| `zephyr/module.yml` | Zephyr module config |
| `samples/canopennode/` | Sample app with OD and LEDs |
| `samples/program_download/` | Firmware update over CAN with MCUboot (`nucleo_f767zi`) |

License: **Apache-2.0**

### What is NOT included

The sample is kept simple on purpose. These features are left out so each project
can add them the way they need:

- **NVS storage** - saving the Object Dictionary to flash is not in the default
  sample. If you need it, add `CONFIG_CANOPENNODE_STORAGE=y`, enable the NVS and
  Settings subsystems, and make sure your board has a `storage` partition.
- **Firmware upgrade over CANopen** - supported by the module (see
  [Program download](#program-download-firmware-update-over-can)), but not enabled
  in this sample: it needs MCUboot and extra objects in the OD. See the separate
  `samples/program_download` sample.

The sample ships with `native_sim.conf` as a reference config for simulation builds.

## Program download (firmware update over CAN)

`src/canopen_program.c` implements CiA 302-3 program download on top of MCUboot, so
an image can be flashed with Zephyr's `canopen` west runner. It is the v4 port of
the same feature in the original CANopenNodeZephyr.

Requirements:

1. Build with sysbuild and MCUboot (`SB_CONFIG_BOOTLOADER_MCUBOOT=y` in
   `sysbuild.conf`).
2. `prj.conf`:

   ```
   CONFIG_BOOTLOADER_MCUBOOT=y
   CONFIG_CANOPENNODE_PROGRAM_DOWNLOAD=y
   CONFIG_FLASH=y
   CONFIG_FLASH_MAP=y
   CONFIG_STREAM_FLASH=y
   CONFIG_MPU_ALLOW_FLASH_WRITE=y   # on boards with an MPU
   ```

3. These objects in the application OD (ARRAY, sub-index 0 = 1):

   | Index:Sub | Type | Access | Meaning |
   |---|---|---|---|
   | 0x1F50:01 | DOMAIN | wo | Program data |
   | 0x1F51:01 | UNSIGNED8 | rw | Program control (0 stop, 1 start, 3 clear, 0x80 confirm) |
   | 0x1F56:01 | UNSIGNED32 | ro | Program software ID (CRC32 of the image) |
   | 0x1F57:01 | UNSIGNED32 | ro | Flash status |

4. After every `CO_CANopenInit()`:

   ```c
   #include <canopennode.h>

   canopen_program_download_attach(OD, CO->NMT, CO->em);
   ```

   It returns `-ENOENT` if one of the objects above is missing.

5. When `CO_process()` returns `CO_RESET_APP` (sent after a "start" command with a
   new image), reboot with `sys_reboot()` so MCUboot can swap the image.

Flash with the runner (needs the Python packages `canopen` and `tqdm`):

```bash
west flash --runner canopen --domain <app> --node-id <id>
```

The module registers the `canopen` runner for any board when
`CONFIG_CANOPENNODE_PROGRAM_DOWNLOAD=y` (most `board.cmake` files do not), with
`--sdo-timeout=30 --timeout=60` and, if the app defines `CONFIG_CANOPEN_NODE_ID`,
`--node-id` set from it. With sysbuild, `--domain <app>` keeps west from also
sending MCUboot over CAN.

A complete example for `nucleo_f767zi` is in
[`samples/program_download`](samples/program_download/README.md).

Commands are only accepted in NMT pre-operational; the runner switches the node to
pre-operational itself. The new image boots as a test image and is confirmed by the
runner afterwards; if it is not confirmed, MCUboot reverts on the next reset.

Clearing the image slot runs inside one SDO transfer and can take several seconds on
large slots, which is why the registered defaults use long timeouts.

---


## How to add it to your Zephyr workspace

### Method 1: As a West project

Add this to your workspace `west.yml`:

```yaml
manifest:
  projects:
    - name: canopennodezephyr
      url: https://github.com/zephyrproject-rtos/CANopenNodeZephyr.git
      revision: main
      submodules:
        - path: CANopenNode
      path: custom/canopennodezephyr
```

Then:

```bash
west update
```

### Method 2: As a submanifest

Make `zephyr/submanifests/canopennodezephyr.yaml` with same content as above,
then `west update`.

## Build and flash

### Quick start for any board with CAN

```bash
west build -b <board> custom/canopennodezephyr/samples/canopennode
west flash
```

### NXP FRDM-MCXN947

Has onboard CAN (FlexCAN). Build and flash:

```bash
west build -p always -b frdm_mcxn947/mcxn947/cpu0 \
    custom/canopennodezephyr/samples/canopennode
west flash
```

User LEDs: `led0` = red, `led1` = green (CANopen status LEDs per CiA 303-3).


---

## Config options

All from `Kconfig`. Set in `prj.conf` or with `-DCONFIG_<NAME>=<value>` on the build line.

| Option | Type | Default | What it does |
|---|---|---|---|
| `CANOPENNODE` | bool | n | Enable CANopenNode |
| `CANOPENNODE_SDO_BUFFER_SIZE` | int | 32 | SDO server buffer size (8-889 bytes) |
| `CANOPENNODE_TRACE_BUFFER_SIZE` | int | 100 | Trace buffer in bytes |
| `CANOPENNODE_TX_WORKQUEUE_STACK_SIZE` | int | 512 | TX workqueue thread stack |
| `CANOPENNODE_TX_WORKQUEUE_PRIORITY` | int | 0 / -1 | TX workqueue priority |
| `CANOPENNODE_STORAGE` | bool | y* | Store OD in NVS (needs `SETTINGS`, off in default sample) |
| `CANOPENNODE_STORAGE_HANDLER_ERASES_EEPROM` | bool | n | Erase EEPROM on write to 0x1011:01 |
| `CANOPENNODE_LEDS` | bool | y | CiA 303-3 LED indicators |
| `CANOPENNODE_LEDS_BICOLOR` | bool | n | Handle LEDs as one bicolor LED |
| `CANOPENNODE_SYNC_THREAD` | bool | n | Separate SYNC thread |
| `CANOPENNODE_PROGRAM_DOWNLOAD` | bool | y | CiA 302-3 program download (needs `BOOTLOADER_MCUBOOT`) |
| `CANOPEN_NODE_ID` | int | 10 | CANopen Node ID (1-127) |

### Example: change node ID, turn off LEDs

```bash
west build -b frdm_mcxn947/mcxn947/cpu0 \
    custom/canopennodezephyr/samples/canopennode \
    -- -DCONFIG_CANOPEN_NODE_ID=42 -DCONFIG_CANOPENNODE_LEDS=n
```

## License

Apache-2.0, see [LICENSE](LICENSE).
