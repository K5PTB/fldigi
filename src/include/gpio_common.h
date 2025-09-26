// ----------------------------------------------------------------------------
// gpio_common.h  --  GPIO control functions
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

#ifndef GPIO_COMMON_H
#define GPIO_COMMON_H

#include <config.h>

#if USE_LIBGPIOD

#include <gpiod.h>
#include <stdint.h>

// Types

typedef uint16_t gpio_num_t;

// Return values
#define GPIO_COMMON_UNKNOWN           UINT16_MAX
#define GPIO_COMMON_OK                0
#define GPIO_COMMON_ERR               -1

// Public functions

void gpio_common_init(void);
gpio_num_t gpio_common_open_line(const char *chip_name, unsigned int line, bool active_low);
int gpio_common_close(gpio_num_t line_num);
int gpio_common_set(gpio_num_t gpio_num, bool val);

#endif // USE_LIBGPIOD

#endif
