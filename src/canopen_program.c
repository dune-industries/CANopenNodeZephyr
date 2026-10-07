/*
 * Copyright (c) 2020 Vestas Wind Systems A/S
 * Copyright (c) 2026 Ported to CANopenNode v4.x
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <CANopen.h>

#include <canopennode.h>
#include <zephyr/dfu/flash_img.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/crc.h>

#define LOG_LEVEL CONFIG_CANOPENNODE_LOG_LEVEL
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(canopen_program);

/* Object dictionary indexes */
#define OD_H1F50_PROGRAM_DATA 0x1F50
#define OD_H1F51_PROGRAM_CTRL 0x1F51
#define OD_H1F56_PROGRAM_SWID 0x1F56
#define OD_H1F57_FLASH_STATUS 0x1F57

/* Program number (sub-index) handled by this implementation */
#define PROGRAM_NUMBER 1U

/* Common program control commands and status */
#define PROGRAM_CTRL_STOP           0x00
#define PROGRAM_CTRL_START          0x01
#define PROGRAM_CTRL_RESET          0x02
#define PROGRAM_CTRL_CLEAR          0x03
/* Zephyr specific program control and status */
#define PROGRAM_CTRL_ZEPHYR_CONFIRM 0x80

/* Flash status bits */
#define FLASH_STATUS_IN_PROGRESS          BIT(0)
/* Flash common error bits values */
#define FLASH_STATUS_NO_ERROR            (0U << 1U)
#define FLASH_STATUS_NO_VALID_PROGRAM    (1U << 1U)
#define FLASH_STATUS_DATA_FORMAT_UNKNOWN (2U << 1U)
#define FLASH_STATUS_DATA_FORMAT_ERROR   (3U << 1U)
#define FLASH_STATUS_FLASH_NOT_CLEARED   (4U << 1U)
#define FLASH_STATUS_FLASH_WRITE_ERROR   (5U << 1U)
#define FLASH_STATUS_GENERAL_ADDR_ERROR  (6U << 1U)
#define FLASH_STATUS_FLASH_SECURED       (7U << 1U)
#define FLASH_STATUS_UNSPECIFIED_ERROR   (63U << 1)

struct canopen_program_context {
	uint32_t flash_status;
	CO_NMT_t *nmt;
	CO_EM_t *em;
	struct flash_img_context flash_img_ctx;
	uint8_t program_status;
	/* Slot1 erased by "clear" and no download started into it since */
	bool flash_erased;
	bool flash_written;
	OD_extension_t ext_1f50;
	OD_extension_t ext_1f51;
	OD_extension_t ext_1f56;
	OD_extension_t ext_1f57;
};

static struct canopen_program_context ctx;

static inline void canopen_program_leds(bool in_progress)
{
	if (IS_ENABLED(CONFIG_CANOPENNODE_LEDS)) {
		canopen_leds_program_download(in_progress);
	}
}

static void canopen_program_set_status(uint32_t status)
{
	ctx.program_status = status;
}

static uint32_t canopen_program_get_status(void)
{
	/*
	 * Non-confirmed boot image takes precedence over other
	 * status. This must be checked on every invocation since the
	 * app may be using other means of confirming the image.
	 */
	if (!boot_is_img_confirmed()) {
		return PROGRAM_CTRL_ZEPHYR_CONFIRM;
	}

	return ctx.program_status;
}

/* Read a 32-bit value into an SDO buffer */
static ODR_t canopen_program_read_u32(uint32_t value, void *buf, OD_size_t count,
				      OD_size_t *countRead)
{
	if (count < sizeof(uint32_t)) {
		return ODR_DEV_INCOMPAT;
	}

	*countRead = CO_setUint32(buf, value);

	return ODR_OK;
}

static ODR_t canopen_odf_1f50_read(OD_stream_t *stream, void *buf, OD_size_t count,
				   OD_size_t *countRead)
{
	if (stream->subIndex != PROGRAM_NUMBER) {
		return OD_readOriginal(stream, buf, count, countRead);
	}

	return ODR_WRITEONLY;
}

/*
 * Called by the SDO server for every chunk of the program data. The data
 * length of a DOMAIN is only known (stream->dataLength != 0) on the final
 * call, which is how the last segment is detected. The SDO server itself
 * verifies the total size against the size indicated by the client.
 */
static ODR_t canopen_odf_1f50_write(OD_stream_t *stream, const void *buf, OD_size_t count,
				    OD_size_t *countWritten)
{
	bool last;
	int err;

	if (stream->subIndex != PROGRAM_NUMBER) {
		return OD_writeOriginal(stream, buf, count, countWritten);
	}

	if (canopen_program_get_status() != PROGRAM_CTRL_CLEAR) {
		ctx.flash_status = FLASH_STATUS_FLASH_NOT_CLEARED;
		return ODR_DATA_DEV_STATE;
	}

	if (stream->dataOffset == 0U) {
		/*
		 * Each download needs a fresh "clear". A download that was aborted or
		 * failed has already programmed part of the slot, and writing over
		 * programmed flash without an erase corrupts the image.
		 */
		if (!ctx.flash_erased) {
			ctx.flash_status = FLASH_STATUS_FLASH_NOT_CLEARED;
			return ODR_DATA_DEV_STATE;
		}
		ctx.flash_erased = false;

		err = flash_img_init(&ctx.flash_img_ctx);
		if (err) {
			LOG_ERR("failed to initialize flash img (err %d)", err);
			CO_errorReport(ctx.em, CO_EM_NON_VOLATILE_MEMORY,
				       CO_EMC_HARDWARE, err);
			ctx.flash_status = FLASH_STATUS_FLASH_WRITE_ERROR;
			return ODR_HW;
		}
		ctx.flash_status = FLASH_STATUS_IN_PROGRESS;
		canopen_program_leds(true);
		LOG_DBG("program download started");
	}

	last = (stream->dataLength != 0U) &&
	       ((stream->dataOffset + count) >= stream->dataLength);

	err = flash_img_buffered_write(&ctx.flash_img_ctx, buf, count, last);
	if (err) {
		LOG_ERR("failed to write flash img (err %d)", err);
		CO_errorReport(ctx.em, CO_EM_NON_VOLATILE_MEMORY,
			       CO_EMC_HARDWARE, err);
		ctx.flash_status = FLASH_STATUS_FLASH_WRITE_ERROR;
		canopen_program_leds(false);
		stream->dataOffset = 0U;
		return ODR_HW;
	}

	*countWritten = count;

	if (!last) {
		stream->dataOffset += count;
		return ODR_PARTIAL;
	}

	LOG_DBG("program downloaded (%zu bytes)",
		flash_img_bytes_written(&ctx.flash_img_ctx));
	stream->dataOffset = 0U;
	ctx.flash_written = true;
	ctx.flash_status = FLASH_STATUS_NO_ERROR;
	canopen_program_set_status(PROGRAM_CTRL_STOP);
	canopen_program_leds(false);

	return ODR_OK;
}

static inline ODR_t canopen_program_cmd_stop(void)
{
	if (canopen_program_get_status() == PROGRAM_CTRL_ZEPHYR_CONFIRM) {
		return ODR_DATA_DEV_STATE;
	}

	LOG_DBG("program stopped");
	canopen_program_set_status(PROGRAM_CTRL_STOP);

	return ODR_OK;
}

static inline ODR_t canopen_program_cmd_start(void)
{
	int err;

	if (canopen_program_get_status() == PROGRAM_CTRL_ZEPHYR_CONFIRM) {
		return ODR_DATA_DEV_STATE;
	}

	if (ctx.flash_written) {
		LOG_DBG("requesting upgrade and reset");

		err = boot_request_upgrade(BOOT_UPGRADE_TEST);
		if (err) {
			LOG_ERR("failed to request upgrade (err %d)", err);
			CO_errorReport(ctx.em, CO_EM_NON_VOLATILE_MEMORY,
				       CO_EMC_HARDWARE, err);
			return ODR_HW;
		}

		/*
		 * CO_process() runs NMT before the SDO server, so the reset is
		 * picked up on the next call, after this SDO response is sent.
		 */
		CO_NMT_sendInternalCommand(ctx.nmt, CO_NMT_RESET_NODE);
	} else {
		LOG_DBG("program started");
		canopen_program_set_status(PROGRAM_CTRL_START);
	}

	return ODR_OK;
}

static inline ODR_t canopen_program_cmd_clear(void)
{
	int err;

	if (canopen_program_get_status() != PROGRAM_CTRL_STOP) {
		return ODR_DATA_DEV_STATE;
	}

	if (!IS_ENABLED(CONFIG_IMG_ERASE_PROGRESSIVELY)) {
		LOG_DBG("erasing flash area");

		err = boot_erase_img_bank(PARTITION_ID(slot1_partition));
		if (err) {
			LOG_ERR("failed to erase image bank (err %d)", err);
			CO_errorReport(ctx.em, CO_EM_NON_VOLATILE_MEMORY,
				       CO_EMC_HARDWARE, err);
			return ODR_HW;
		}
	}

	LOG_DBG("program cleared");
	canopen_program_set_status(PROGRAM_CTRL_CLEAR);
	ctx.flash_status = FLASH_STATUS_NO_ERROR;
	ctx.flash_erased = true;
	ctx.flash_written = false;

	return ODR_OK;
}

static inline ODR_t canopen_program_cmd_confirm(void)
{
	int err;

	if (canopen_program_get_status() == PROGRAM_CTRL_ZEPHYR_CONFIRM) {
		err = boot_write_img_confirmed();
		if (err) {
			LOG_ERR("failed to confirm image (err %d)", err);
			CO_errorReport(ctx.em, CO_EM_NON_VOLATILE_MEMORY,
				       CO_EMC_HARDWARE, err);
			return ODR_HW;
		}

		LOG_DBG("program confirmed");
		canopen_program_set_status(PROGRAM_CTRL_START);
	}

	return ODR_OK;
}

static ODR_t canopen_odf_1f51_read(OD_stream_t *stream, void *buf, OD_size_t count,
				   OD_size_t *countRead)
{
	if (stream->subIndex != PROGRAM_NUMBER) {
		return OD_readOriginal(stream, buf, count, countRead);
	}

	if (count < sizeof(uint8_t)) {
		return ODR_DEV_INCOMPAT;
	}

	*countRead = CO_setUint8(buf, canopen_program_get_status());

	return ODR_OK;
}

static ODR_t canopen_odf_1f51_write(OD_stream_t *stream, const void *buf, OD_size_t count,
				    OD_size_t *countWritten)
{
	ODR_t odr;
	uint8_t cmd;

	if (stream->subIndex != PROGRAM_NUMBER) {
		return OD_writeOriginal(stream, buf, count, countWritten);
	}

	if (count != sizeof(uint8_t)) {
		return ODR_TYPE_MISMATCH;
	}

	if (CO_NMT_getInternalState(ctx.nmt) != CO_NMT_PRE_OPERATIONAL) {
		LOG_DBG("not in pre-operational state");
		return ODR_DATA_DEV_STATE;
	}

	/* The OD variable is not updated, reads always return the live status */
	cmd = CO_getUint8(buf);

	LOG_DBG("program status = %d, cmd = %d", canopen_program_get_status(),
		cmd);

	switch (cmd) {
	case PROGRAM_CTRL_STOP:
		odr = canopen_program_cmd_stop();
		break;
	case PROGRAM_CTRL_START:
		odr = canopen_program_cmd_start();
		break;
	case PROGRAM_CTRL_CLEAR:
		odr = canopen_program_cmd_clear();
		break;
	case PROGRAM_CTRL_ZEPHYR_CONFIRM:
		odr = canopen_program_cmd_confirm();
		break;
	case PROGRAM_CTRL_RESET:
		__fallthrough;
	default:
		LOG_DBG("unsupported command '%d'", cmd);
		odr = ODR_INVALID_VALUE;
	}

	if (odr == ODR_OK) {
		*countWritten = count;
	}

	return odr;
}

/** @brief Calculate crc for region in flash
 *
 * @param flash_area Flash area to read from, must be open
 * @offset Offset to read from
 * @size Number of bytes to include in calculation
 * @pcrc Pointer to uint32_t where crc will be written if return value is 0
 *
 * @return 0 if successful, negative errno on failure
 */
static int flash_crc(const struct flash_area *flash_area,
		off_t offset, size_t size, uint32_t *pcrc)
{
	uint32_t crc = 0;
	uint8_t buffer[32];

	while (size > 0) {
		size_t len = MIN(size, sizeof(buffer));

		int err = flash_area_read(flash_area, offset, buffer, len);

		if (err) {
			return err;
		}

		crc = crc32_ieee_update(crc, buffer, len);

		offset += len;
		size -= len;
	}

	*pcrc = crc;

	return 0;
}

/*
 * Calculate the CRC32 of the image that is running or will be started upon
 * receiving the next 'start' command. Returns 0 as CRC if there is no
 * valid MCUboot header.
 */
static int canopen_program_swid(uint32_t *crc)
{
	const struct flash_area *flash_area;
	struct mcuboot_img_header header;
	size_t offset;
	uint8_t fa_id;
	int err;

	if (ctx.flash_written) {
		fa_id = PARTITION_ID(slot1_partition);
	} else {
		fa_id = PARTITION_ID(slot0_partition);
	}

	*crc = 0U;

	err = boot_read_bank_header(fa_id, &header, sizeof(header));
	if (err) {
		LOG_WRN("failed to read bank header (err %d)", err);
		return 0;
	}

	if (header.mcuboot_version != 1) {
		LOG_WRN("unsupported mcuboot header version %d",
			header.mcuboot_version);
		return 0;
	}

	/*
	 * The image does not always start at offset 0 of the slot (e.g. the
	 * secondary slot in MCUboot swap-using-offset mode starts one sector in).
	 */
	offset = boot_get_image_start_offset(fa_id);

	err = flash_area_open(fa_id, &flash_area);
	if (err) {
		LOG_ERR("failed to open flash area (err %d)", err);
		return err;
	}

	if ((offset > flash_area->fa_size) ||
	    (header.h.v1.image_size > (flash_area->fa_size - offset))) {
		LOG_WRN("image size %u exceeds flash area", header.h.v1.image_size);
		flash_area_close(flash_area);
		return 0;
	}

	err = flash_crc(flash_area, offset, header.h.v1.image_size, crc);

	flash_area_close(flash_area);

	if (err) {
		LOG_ERR("failed to read flash (err %d)", err);
	}

	return err;
}

static ODR_t canopen_odf_1f56_read(OD_stream_t *stream, void *buf, OD_size_t count,
				   OD_size_t *countRead)
{
	uint32_t crc;
	int err;

	if (stream->subIndex != PROGRAM_NUMBER) {
		return OD_readOriginal(stream, buf, count, countRead);
	}

	/*
	 * Reading from flash and calculating crc can take 100ms or more, and
	 * this function is called by the SDO server with the OD lock taken.
	 * Release the lock (a recursive mutex owned by this thread on Zephyr)
	 * so the SYNC thread is not stalled, and reacquire before return.
	 */
	CO_UNLOCK_OD(NULL);
	err = canopen_program_swid(&crc);
	CO_LOCK_OD(NULL);

	if (err) {
		CO_errorReport(ctx.em, CO_EM_NON_VOLATILE_MEMORY,
			       CO_EMC_HARDWARE, err);
		return ODR_HW;
	}

	return canopen_program_read_u32(crc, buf, count, countRead);
}

static ODR_t canopen_odf_1f57_read(OD_stream_t *stream, void *buf, OD_size_t count,
				   OD_size_t *countRead)
{
	if (stream->subIndex != PROGRAM_NUMBER) {
		return OD_readOriginal(stream, buf, count, countRead);
	}

	return canopen_program_read_u32(ctx.flash_status, buf, count, countRead);
}

static ODR_t canopen_odf_read_only(OD_stream_t *stream, const void *buf, OD_size_t count,
				   OD_size_t *countWritten)
{
	if (stream->subIndex != PROGRAM_NUMBER) {
		return OD_writeOriginal(stream, buf, count, countWritten);
	}

	return ODR_READONLY;
}

static int canopen_program_extension_init(OD_t *od, uint16_t index, OD_extension_t *ext,
					  ODR_t (*read)(OD_stream_t *, void *, OD_size_t,
							OD_size_t *),
					  ODR_t (*write)(OD_stream_t *, const void *, OD_size_t,
							 OD_size_t *))
{
	OD_entry_t *entry = OD_find(od, index);

	if (entry == NULL) {
		LOG_ERR("object dictionary is missing index 0x%04X", index);
		return -ENOENT;
	}

	ext->object = &ctx;
	ext->read = read;
	ext->write = write;

	if (OD_extension_init(entry, ext) != ODR_OK) {
		LOG_ERR("failed to extend index 0x%04X", index);
		return -EINVAL;
	}

	return 0;
}

int canopen_program_download_attach(OD_t *od, CO_NMT_t *nmt, CO_EM_t *em)
{
	int err;

	canopen_program_set_status(PROGRAM_CTRL_START);
	ctx.flash_status = FLASH_STATUS_NO_ERROR;
	ctx.flash_erased = false;
	ctx.flash_written = false;
	ctx.nmt = nmt;
	ctx.em = em;

	err = canopen_program_extension_init(od, OD_H1F50_PROGRAM_DATA, &ctx.ext_1f50,
					     canopen_odf_1f50_read, canopen_odf_1f50_write);
	if (err) {
		return err;
	}

	err = canopen_program_extension_init(od, OD_H1F51_PROGRAM_CTRL, &ctx.ext_1f51,
					     canopen_odf_1f51_read, canopen_odf_1f51_write);
	if (err) {
		return err;
	}

	err = canopen_program_extension_init(od, OD_H1F56_PROGRAM_SWID, &ctx.ext_1f56,
					     canopen_odf_1f56_read, canopen_odf_read_only);
	if (err) {
		return err;
	}

	return canopen_program_extension_init(od, OD_H1F57_FLASH_STATUS, &ctx.ext_1f57,
					      canopen_odf_1f57_read, canopen_odf_read_only);
}
