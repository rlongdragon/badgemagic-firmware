#include "data.h"
#include "power.h"
#include "leddrv.h"
#include "debug.h"
#include "legacyctrl.h"
#include "ngctrl.h"
#include "CH58x_common.h"
#include "usb/usb.h"

static uint16_t auth_code = 0;
static uint8_t  authorized = 0;

void legacy_set_auth_code(uint16_t code)
{
	auth_code  = code;
	authorized = 0;
}

void legacy_reset_auth()
{
	authorized = 0;
}

static uint16_t legacy_usb_rx_len;
static uint16_t legacy_usb_data_len;
static uint8_t *legacy_usb_data;
static uint8_t legacy_usb_active;

#define NG_USB_MAX_LEN (1 + LED_COLS * 2)
static uint8_t ng_usb_buf[NG_USB_MAX_LEN];
static uint16_t ng_usb_len;
static uint16_t ng_usb_expected;

int legacy_ble_rx(uint8_t *val, uint16_t len)
{
	_TRACE();
	static uint16_t c, data_len, n;
	static uint8_t *data;

	if (len != LEGACY_TRANSFER_WIDTH) {
		char buf[32];
		int blen = snprintf(buf, sizeof(buf), "BLE: width mismatch %d\n", len);
		cdc_tx_poll((uint8_t *)buf, blen, 10);
		return -1;
	}

	if (!authorized) {
		if (!memcmp(val, "wang", 4)) {
			char buf[32];
			int blen = snprintf(buf, sizeof(buf), "BLE: rejected - not authed\n");
			cdc_tx_poll((uint8_t *)buf, blen, 10);
			return -4;
		}

		if (len >= 4
			&& val[0] >= '0' && val[0] <= '9'
			&& val[1] >= '0' && val[1] <= '9'
			&& val[2] >= '0' && val[2] <= '9'
			&& val[3] >= '0' && val[3] <= '9')
		{
			uint16_t attempt = (val[0] - '0') * 1000
			                 + (val[1] - '0') * 100
			                 + (val[2] - '0') * 10
			                 + (val[3] - '0');

			if (attempt == auth_code) {
				authorized = 1;
				char buf[32];
				int blen = snprintf(buf, sizeof(buf), "BLE: auth OK\n");
				cdc_tx_poll((uint8_t *)buf, blen, 10);
				return 0;
			} else {
				char buf[48];
				int blen = snprintf(buf, sizeof(buf), "BLE: wrong code got=%04d exp=%04d\n", attempt, auth_code);
				cdc_tx_poll((uint8_t *)buf, blen, 10);
				return -5;
			}
		}

		char buf[32];
		int blen = snprintf(buf, sizeof(buf), "BLE: invalid code attempt\n");
		cdc_tx_poll((uint8_t *)buf, blen, 10);
		return -5;
	}

	// Authorized path — log transfer progress over CDC
	{
		char buf[48];
		int blen = snprintf(buf, sizeof(buf), "BLE: rx packet c=%d len=%d\n", c, len);
		cdc_tx_poll((uint8_t *)buf, blen, 10);
	}

	if (c == 0) {
		if (memcmp(val, "wang", 5)) {
			char buf[32];
			int blen = snprintf(buf, sizeof(buf), "BLE: not a header\n");
			cdc_tx_poll((uint8_t *)buf, blen, 10);
			return -2;
		} else {
			free(data);
			data = malloc(sizeof(data_legacy_t));
			if (!data) {
				char buf[32];
				int blen = snprintf(buf, sizeof(buf), "BLE: malloc failed\n");
				cdc_tx_poll((uint8_t *)buf, blen, 10);
				return -3;
			}
		}
	} else {
		if (!memcmp(val, "wang", 5)) {
			free(data);
			c = 0;
			data = malloc(sizeof(data_legacy_t));
			if (!data) {
				char buf[32];
				int blen = snprintf(buf, sizeof(buf), "BLE: malloc failed\n");
				cdc_tx_poll((uint8_t *)buf, blen, 10);
				return -3;
			}
		}
	}

	memcpy(data + c * len, val, len);

	if (c == 1) {
		data_legacy_t *d = (data_legacy_t *)data;
		n = bigendian16_sum(d->sizes, 8);
		data_len = LEGACY_HEADER_SIZE + LED_ROWS * n;
		char buf[48];
		int blen = snprintf(buf, sizeof(buf), "BLE: data_len=%d\n", data_len);
		cdc_tx_poll((uint8_t *)buf, blen, 10);
		data = realloc(data, data_len);
		if (!data) {
			char buf2[32];
			int blen2 = snprintf(buf2, sizeof(buf2), "BLE: realloc failed\n");
			cdc_tx_poll((uint8_t *)buf2, blen2, 10);
			return -3;
		}
	}

	if (c > 2 && ((c+1) * LEGACY_TRANSFER_WIDTH) >= data_len) {
		char buf[32];
		int blen = snprintf(buf, sizeof(buf), "BLE: transfer complete\n");
		cdc_tx_poll((uint8_t *)buf, blen, 10);
		data_legacy_t *d = (data_legacy_t *)data;
		RTC_InitTime(2000 + ((d->timestamp[0] - 208 + 256) % 256), d->timestamp[1], d->timestamp[2], d->timestamp[3], d->timestamp[4], d->timestamp[5]);
		data_flatSave(data, data_len);
		free(data);
		data = NULL;
		handle_after_rx();
	}

	c++;
	return 0;
}

int legacy_usb_rx(uint8_t *buf, uint16_t len)
{
	PRINT("dump first 8 bytes: %02x %02x %02x %02x %02x %02x %02x %02x\n",
				buf[0], buf[1], buf[2], buf[3],
				buf[4], buf[5], buf[6], buf[7]);

	if (legacy_usb_rx_len == 0) {
		if (memcmp(buf, "wang", 5))
			return -1;

		legacy_usb_active = 1;
		legacy_usb_data_len = 0;

		int init_len = len > LEGACY_HEADER_SIZE ? len : sizeof(data_legacy_t);
		init_len += MAX_PACKET_SIZE;
		legacy_usb_data = malloc(init_len);
		if (!legacy_usb_data) {
			legacy_usb_active = 0;
			return -3;
		}
	}

	memcpy(legacy_usb_data + legacy_usb_rx_len, buf, len);
	legacy_usb_rx_len += len;

	if (!legacy_usb_data_len) {
		data_legacy_t *d = (data_legacy_t *)legacy_usb_data;
		uint16_t n = bigendian16_sum(d->sizes, 8);
		legacy_usb_data_len = LEGACY_HEADER_SIZE + LED_ROWS * n;
		legacy_usb_data = realloc(legacy_usb_data, legacy_usb_data_len);
		if (!legacy_usb_data) {
			legacy_usb_rx_len = 0;
			legacy_usb_data_len = 0;
			legacy_usb_active = 0;
			return -3;
		}
	}

	if ((legacy_usb_rx_len > LEGACY_HEADER_SIZE)
			&& legacy_usb_rx_len >= legacy_usb_data_len) {
		data_flatSave(legacy_usb_data, legacy_usb_data_len);
		free(legacy_usb_data);
		legacy_usb_data = NULL;
		legacy_usb_rx_len = 0;
		legacy_usb_data_len = 0;
		legacy_usb_active = 0;
		handle_after_rx();
	}
	return 0;
}

void legacy_bypass_auth()
{
    authorized = 1;   // skip auth entirely if KEY4 is pressed
}

static int ng_usb_rx(uint8_t *buf, uint16_t len)
{
	if (len == 0)
		return 0;

	if (ng_usb_len == 0) {
		uint8_t cmd = buf[0];
		if (cmd == 0x03) {
			ng_usb_expected = NG_USB_MAX_LEN;
		} else {
			ng_usb_expected = len;
		}
	}

	if (ng_usb_len + len > NG_USB_MAX_LEN) {
		ng_usb_len = 0;
		ng_usb_expected = 0;
		return -2;
	}

	memcpy(ng_usb_buf + ng_usb_len, buf, len);
	ng_usb_len += len;

	if (ng_usb_expected && ng_usb_len >= ng_usb_expected) {
		ng_parse(ng_usb_buf, ng_usb_expected);
		ng_usb_len = 0;
		ng_usb_expected = 0;
	} else if (ng_usb_expected == len) {
		ng_parse(ng_usb_buf, ng_usb_len);
		ng_usb_len = 0;
		ng_usb_expected = 0;
	}

	return 0;
}

int usb_rx_dispatch(uint8_t *buf, uint16_t len)
{
	if (legacy_usb_active || (len >= 5 && !memcmp(buf, "wang", 5))) {
		return legacy_usb_rx(buf, len);
	}

	return ng_usb_rx(buf, len);
}