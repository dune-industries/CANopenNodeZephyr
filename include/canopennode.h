/*
 * Copyright (c) 2019 Vestas Wind Systems A/S
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @brief Zephyr glue API for the CANopenNode v4.x stack
 * @defgroup canopen CANopen Network Stack
 * @{
 */

#ifndef ZEPHYR_MODULES_CANOPENNODE_CANOPENNODE_H_
#define ZEPHYR_MODULES_CANOPENNODE_CANOPENNODE_H_

#include <CANopen.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Attach CANopen object dictionary program download handlers.
 *
 * Attach CiA 302-3 program download handlers to object dictionary
 * indexes 0x1F50 (program data), 0x1F51 (program control), 0x1F56
 * (program software identification) and 0x1F57 (flash status). All four
 * objects must exist in the application object dictionary as ARRAY
 * objects with sub-index 1 present:
 *
 * - 0x1F50:01 DOMAIN, SDO write
 * - 0x1F51:01 UNSIGNED8, SDO read/write
 * - 0x1F56:01 UNSIGNED32, SDO read
 * - 0x1F57:01 UNSIGNED32, SDO read
 *
 * This function must be called after `CO_CANopenInit()`, on every
 * communication reset.
 *
 * @param od  Application object dictionary
 * @param nmt CANopenNode NMT object
 * @param em  CANopenNode Emergency object
 *
 * @retval 0 on success
 * @retval -ENOENT if one of the objects is missing from @p od
 * @retval -EINVAL if an object could not be extended
 */
int canopen_program_download_attach(OD_t *od, CO_NMT_t *nmt, CO_EM_t *em);

/**
 * @brief Initialize CANopen LED indicators.
 *
 * Initialize CANopen LED indicators and attach callbacks for setting
 * their state. Two LED indicators, a red and a green, are supported
 * according to CiA 303-3.
 *
 * @param nmt CANopenNode NMT object.
 * @param green_cb callback for changing state on the green LED indicator.
 * @param green_arg argument to pass to the green LED indicator callback.
 * @param red_cb callback for changing state on the red LED indicator.
 * @param red_arg argument to pass to the red LED indicator callback.
 */
void canopen_leds_init(CO_NMT_t *nmt,
		       canopen_led_callback_t green_cb, void *green_arg,
		       canopen_led_callback_t red_cb, void *red_arg);

/**
 * @brief Indicate CANopen program download in progress
 *
 * @param in_progress true if program download is in progress, false otherwise
 */
void canopen_leds_program_download(bool in_progress);

#ifdef __cplusplus
}
#endif

/**
 * @}
 */

#endif /* ZEPHYR_MODULES_CANOPENNODE_CANOPENNODE_H_ */
