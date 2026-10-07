# CANopenNode program download sample

Firmware update over CAN with CANopenNode v4.x and MCUboot, using CiA 302-3
program download and Zephyr's `canopen` west flash runner.

Tested on `nucleo_f767zi` (bxCAN on CAN1, PD0/PD1, 500 kbit/s). Other boards
need MCUboot partitions (`slot0_partition`, `slot1_partition`), a
`zephyr,canbus` chosen node, and `CONFIG_MPU_ALLOW_FLASH_WRITE=y` if they
have an MPU (see `boards/`).

## What it does

- Runs CANopenNode as node 10 (`CONFIG_CANOPEN_NODE_ID`) with a 1 s heartbeat
  (`70A [1] 05` when operational).
- Provides the program download objects 0x1F50, 0x1F51, 0x1F56 and 0x1F57
  in its object dictionary (`src/OD.eds`, generated `src/OD.c`/`src/OD.h`).
- Logs the running image version at boot and blinks `led0` every
  `LED_BLINK_MS` (in `src/main.c`), so you can see a new image take over.

## Build

From the workspace root:

```bash
west build -p always -b nucleo_f767zi --sysbuild \
    modules/canopennodezephyr/samples/program_download
```

`--sysbuild` builds MCUboot and the signed application together.

## First flash (USB)

```bash
west flash
```

This flashes MCUboot and the application. MCUboot cannot be updated over CAN.

## Flash over CAN

Bring up the host CAN interface and tell python-can which one to use:

```bash
sudo ip link set can0 up type can bitrate 500000
export CAN_INTERFACE=socketcan CAN_CHANNEL=can0
```

The runner needs the Python packages `canopen` and `tqdm`.

Make a visible change (for example `LED_BLINK_MS` in `src/main.c`), bump
`CONFIG_MCUBOOT_IMGTOOL_SIGN_VERSION` in `prj.conf`, then:

```bash
west build
west flash --runner canopen --domain program_download
```

- `--domain program_download` flashes only the application. Without it,
  sysbuild also tries to send MCUboot over CAN.
- The node ID (`--node-id`) defaults to `CONFIG_CANOPEN_NODE_ID`. The module
  registers the `canopen` runner with `--sdo-timeout=30 --timeout=60`, since
  erasing the image slot takes several seconds.

The runner puts the node in pre-operational, clears the update slot, downloads
`zephyr.signed.bin`, starts it (the node reboots and MCUboot swaps the image),
and confirms the new image. An image that is not confirmed is reverted by
MCUboot on the next reset.

After a CAN flash the node is left in pre-operational. Start it with
`cansend can0 000#010A` (or reset the board) if you need PDOs.
