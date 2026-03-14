// SPDX-License-Identifier: MIT
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* C wrappers for C++ BoardLed API (called from C modules) */
void board_led_set_state_not_joined(void);
void board_led_set_state_pairing(void);
void board_led_set_state_joined(void);
void board_led_set_state_error(void);

#ifdef __cplusplus
}
#endif
