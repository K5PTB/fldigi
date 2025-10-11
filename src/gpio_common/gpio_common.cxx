// ----------------------------------------------------------------------------
// gpio_common.c  --  GPIO control functions
//
// Copyright (C) 2025
//		Dave Freese, W1HKJ
//		Levente Kovacs, HA5OGL
//
// Fldigi is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// Fldigi is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with fldigi.  If not, see <http://www.gnu.org/licenses/>.
// ----------------------------------------------------------------------------

#include <config.h>

#if USE_LIBGPIOD

#include <gpiod.h>
#include <stdio.h>
#include <unistd.h>

#include <errno.h>
#include <string.h>

#include "gpio_common.h"
#include "debug.h"

#define GPIO_MAX_LINES 32
#define GPIO_CONSUMER "FLDIGI"

//Types
typedef struct gpio_common {
	struct gpiod_line_request *request;
	unsigned int offset;
	bool used;
} gpio_common_t;

// local variables

static gpio_common_t gpio[GPIO_MAX_LINES];

// Function implementations

void gpio_common_init(void) {
	LOG_INFO("Initializing GPIO common structure");
	for (gpio_num_t i = 0; i < GPIO_MAX_LINES; i++) {
		gpio[i].used = false;
	}
}

gpio_num_t gpio_common_open_line(
				const char *chip_name,
				unsigned int line,
				bool active_low)
{
	gpio_num_t gpio_num;
	int ret;

	struct gpiod_request_config *req_cfg = NULL;
	struct gpiod_line_settings *settings;
	struct gpiod_line_config *line_cfg;
	struct gpiod_chip *chip;

	gpio_num = GPIO_COMMON_UNKNOWN;

	if (chip_name == NULL) {
		LOG_ERROR("No chip name supplied");
		return GPIO_COMMON_UNKNOWN;
	}

	LOG_INFO("Opening GPIO line %d on chip %s", line, chip_name);

	// Get a free slot
	for (gpio_num_t i = 0; i < GPIO_MAX_LINES; i++) {
		if (gpio[i].used == false) {
			gpio_num = i;
			break;
		}
	}

	if (gpio_num == GPIO_COMMON_UNKNOWN) {
		LOG_ERROR("Too many GPIO devices open.");
		gpio_num = GPIO_COMMON_UNKNOWN;
		goto out;
	}

	chip = gpiod_chip_open(chip_name);

	if (chip == NULL) {
		LOG_ERROR("Failed to open GPIO chip %s", chip_name);
		gpio_num = GPIO_COMMON_UNKNOWN;
		goto out;
	}

	settings = gpiod_line_settings_new();

	if (settings == NULL) {
		LOG_ERROR("Unable to allocate memory for line settings.");
		gpio_num = GPIO_COMMON_UNKNOWN;
		goto close_chip;
	}

	gpiod_line_settings_set_direction(settings,
						GPIOD_LINE_DIRECTION_OUTPUT);
	gpiod_line_settings_set_output_value(settings, (gpiod_line_value)0);
	gpiod_line_settings_set_active_low(settings, active_low);

	line_cfg = gpiod_line_config_new();

	if (!line_cfg) {
		gpio_num = GPIO_COMMON_UNKNOWN;
		goto free_settings;
	}

	ret = gpiod_line_config_add_line_settings(line_cfg, &line, 1, settings);
	if (ret < 0) {
		LOG_ERROR("Failed to add line settings");
		gpio_num = GPIO_COMMON_UNKNOWN;
		goto free_line_config;
	}

	req_cfg = gpiod_request_config_new();
	if (!req_cfg) {
		goto free_line_config;
	}

	gpiod_request_config_set_consumer(req_cfg, "FLDIGI");

	gpio[gpio_num].request = gpiod_chip_request_lines(chip, req_cfg, line_cfg);

	if (gpio[gpio_num].request == NULL) {
		LOG_ERROR("GPIO line request FAILED %d.", gpio_num);
		gpio_num = GPIO_COMMON_UNKNOWN;
		goto free_line_config;
	} else {
		gpio[gpio_num].used = true;
		gpio[gpio_num].offset = line;
	}

free_line_config:
	gpiod_line_config_free(line_cfg);

free_settings:
	gpiod_line_settings_free(settings);

close_chip:
	gpiod_chip_close(chip);

out:
	return gpio_num;
}


int gpio_common_set(gpio_num_t gpio_num, bool val)
{
	if (gpio_num >= GPIO_MAX_LINES || gpio[gpio_num].request == NULL) {
		return GPIO_COMMON_ERR;
	}
	uint16_t gpiod_val;

	if (val) {
		gpiod_val = GPIOD_LINE_VALUE_ACTIVE;
	} else {
		gpiod_val = GPIOD_LINE_VALUE_INACTIVE;
	}


	int ret = gpiod_line_request_set_value(
			gpio[gpio_num].request,
			gpio[gpio_num].offset,
			(gpiod_line_value)gpiod_val);
	if (ret < 0) {
		LOG_ERROR("Error setting line.");
		return GPIO_COMMON_ERR;
	}
	return GPIO_COMMON_OK;
}


int gpio_common_close(gpio_num_t line_num)
{
	if (line_num >= GPIO_MAX_LINES) {
		return GPIO_COMMON_ERR;
	}

	if (gpio[line_num].request != NULL) {
		LOG_INFO("Releasing GPIO line %d\n", line_num);
		gpiod_line_request_release(gpio[line_num].request);
		gpio[line_num].request = NULL;
		gpio[line_num].used = false;
		gpio[line_num].offset = 0;
	}
	return GPIO_COMMON_OK;
}

#endif // USE_LIBGPIOD
