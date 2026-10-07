/*
 * CANopenNode v4.x sample: firmware update over CAN (CiA 302-3 program
 * download) with MCUboot.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/reboot.h>
#include <canopennode.h>
#include "OD.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(app, LOG_LEVEL_INF);

#define CAN_DEV DEVICE_DT_GET(DT_CHOSEN(zephyr_canbus))
#define BITRATE ((DT_PROP_OR(DT_CHOSEN(zephyr_canbus), bitrate, \
		  DT_PROP_OR(DT_CHOSEN(zephyr_canbus), bus_speed, \
		  CONFIG_CAN_DEFAULT_BITRATE))) / 1000)

#define NMT_CONTROL (CO_NMT_STARTUP_TO_OPERATIONAL | CO_NMT_ERR_ON_ERR_REG | \
		     CO_ERR_REG_GENERIC_ERR | CO_ERR_REG_COMMUNICATION)
#define FIRST_HB_TIME_MS        500
#define SDO_SRV_TIMEOUT_TIME_MS 1000
#define SDO_CLI_TIMEOUT_TIME_MS 500
#define SDO_CLI_BLOCK           false

/* Change this and flash over CAN to see the new image take over. */
#define LED_BLINK_MS 1000

/* Global CANopenNode object, also used by the module's SYNC thread. */
CO_t *CO;

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET_OR(DT_ALIAS(led0), gpios, {0});

static void led_blink(struct k_timer *timer)
{
	ARG_UNUSED(timer);
	gpio_pin_toggle_dt(&led);
}

K_TIMER_DEFINE(led_timer, led_blink, NULL);

static void led_init(void)
{
	if (led.port == NULL || !gpio_is_ready_dt(&led) ||
	    gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE) != 0) {
		return;
	}

	k_timer_start(&led_timer, K_MSEC(LED_BLINK_MS), K_MSEC(LED_BLINK_MS));
}

static void log_image_version(void)
{
	struct mcuboot_img_header header;

	if (boot_read_bank_header(PARTITION_ID(slot0_partition), &header,
				  sizeof(header)) != 0) {
		return;
	}

	LOG_INF("Image version %u.%u.%u+%u%s",
		header.h.v1.sem_ver.major, header.h.v1.sem_ver.minor,
		header.h.v1.sem_ver.revision, header.h.v1.sem_ver.build_num,
		boot_is_img_confirmed() ? "" : " (not confirmed)");
}

static CO_ReturnError_t canopen_init(const struct device *can_dev)
{
	CO_ReturnError_t err;
	uint32_t err_info = 0U;

	CO_CANsetConfigurationMode((void *)can_dev);
	CO_CANmodule_disable(CO->CANmodule);

	err = CO_CANinit(CO, (void *)can_dev, BITRATE);
	if (err != CO_ERROR_NO) {
		LOG_ERR("CO_CANinit failed (err = %d)", err);
		return err;
	}

	err = CO_CANopenInit(CO, NULL, NULL, OD, NULL, NMT_CONTROL,
			     FIRST_HB_TIME_MS, SDO_SRV_TIMEOUT_TIME_MS,
			     SDO_CLI_TIMEOUT_TIME_MS, SDO_CLI_BLOCK,
			     CONFIG_CANOPEN_NODE_ID, &err_info);
	if (err != CO_ERROR_NO && err != CO_ERROR_NODE_ID_UNCONFIGURED_LSS) {
		LOG_ERR("CO_CANopenInit failed (err = %d, info = 0x%x)", err, err_info);
		return err;
	}

	err = CO_CANopenInitPDO(CO, CO->em, OD, CONFIG_CANOPEN_NODE_ID, &err_info);
	if (err != CO_ERROR_NO && err != CO_ERROR_NODE_ID_UNCONFIGURED_LSS) {
		LOG_ERR("CO_CANopenInitPDO failed (err = %d, info = 0x%x)", err, err_info);
		return err;
	}

	if (canopen_program_download_attach(OD, CO->NMT, CO->em) != 0) {
		LOG_ERR("program download attach failed");
	}

	CO_CANsetNormalMode(CO->CANmodule);

	return CO_ERROR_NO;
}

int main(void)
{
	CO_NMT_reset_cmd_t reset = CO_RESET_NOT;
	CO_ReturnError_t err;
	const struct device *can_dev = CAN_DEV;
	uint32_t heap_used = 0U;
	int64_t timestamp;

	led_init();
	log_image_version();

	if (!device_is_ready(can_dev)) {
		LOG_ERR("CAN interface not ready");
		return 0;
	}

	CO = CO_new(NULL, &heap_used);
	if (CO == NULL) {
		LOG_ERR("CO_new failed");
		return 0;
	}

	while (reset != CO_RESET_APP) {
		/*
		 * The SYNC thread processes PDOs under the OD lock. Hold it while
		 * (re)initializing, so a communication reset never rebuilds the
		 * PDO/SYNC objects while that thread is using them.
		 */
		CO_LOCK_OD(CO->CANmodule);
		err = canopen_init(can_dev);
		CO_UNLOCK_OD(CO->CANmodule);
		if (err != CO_ERROR_NO) {
			return 0;
		}

		LOG_INF("CANopen stack initialized (node %d, %d kbit/s)",
			CONFIG_CANOPEN_NODE_ID, BITRATE);

		reset = CO_RESET_NOT;
		timestamp = k_uptime_get();

		while (reset == CO_RESET_NOT) {
			uint32_t elapsed_us = (uint32_t)k_uptime_delta(&timestamp) * USEC_PER_MSEC;

			reset = CO_process(CO, false, elapsed_us, NULL);
			CO_CANmodule_process(CO->CANmodule);

			k_sleep(K_MSEC(1));
		}
	}

	LOG_INF("Resetting device");

	/* Let the last SDO response (e.g. program start) leave the CAN controller. */
	k_sleep(K_MSEC(100));

	/*
	 * No CO_delete(): the SYNC thread may still be using CO, and the
	 * reboot resets everything anyway. MCUboot swaps in a new image now.
	 */
	CO_CANsetConfigurationMode((void *)can_dev);
	sys_reboot(SYS_REBOOT_COLD);

	return 0;
}
