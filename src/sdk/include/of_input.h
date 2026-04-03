/*
 * of_input.h -- Input subsystem API for openfpgaOS
 *
 * 2 controllers, d-pad + ABXY + L/R + sticks + triggers.
 */

#ifndef OF_INPUT_H
#define OF_INPUT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#define OF_MAX_PLAYERS  2

/* Button masks */
#define OF_BTN_UP       (1 << 0)
#define OF_BTN_DOWN     (1 << 1)
#define OF_BTN_LEFT     (1 << 2)
#define OF_BTN_RIGHT    (1 << 3)
#define OF_BTN_A        (1 << 4)
#define OF_BTN_B        (1 << 5)
#define OF_BTN_X        (1 << 6)
#define OF_BTN_Y        (1 << 7)
#define OF_BTN_L1       (1 << 8)
#define OF_BTN_R1       (1 << 9)
#define OF_BTN_L2       (1 << 10)
#define OF_BTN_R2       (1 << 11)
#define OF_BTN_L3       (1 << 12)
#define OF_BTN_R3       (1 << 13)
#define OF_BTN_SELECT   (1 << 14)
#define OF_BTN_START    (1 << 15)

typedef struct {
    uint32_t buttons;
    uint32_t buttons_pressed;
    uint32_t buttons_released;
    int16_t  joy_lx, joy_ly;
    int16_t  joy_rx, joy_ry;
    uint16_t trigger_l, trigger_r;
} of_input_state_t;

#ifndef OF_PC

#include "of_syscall.h"
#include "of_syscall_numbers.h"

static of_input_state_t __of_p0, __of_p1;

static inline void of_input_poll(void) {
    __of_syscall0(OF_SYS_INPUT_POLL);
    __of_syscall2(OF_SYS_INPUT_GET_STATE, 0, (long)&__of_p0);
    __of_syscall2(OF_SYS_INPUT_GET_STATE, 1, (long)&__of_p1);
}

static inline int of_btn(uint32_t mask) {
    return (__of_p0.buttons & mask) != 0;
}

static inline int of_btn_pressed(uint32_t mask) {
    return (__of_p0.buttons_pressed & mask) != 0;
}

static inline int of_btn_released(uint32_t mask) {
    return (__of_p0.buttons_released & mask) != 0;
}

static inline int of_btn_p2(uint32_t mask) {
    return (__of_p1.buttons & mask) != 0;
}

static inline int of_btn_pressed_p2(uint32_t mask) {
    return (__of_p1.buttons_pressed & mask) != 0;
}

static inline int of_btn_released_p2(uint32_t mask) {
    return (__of_p1.buttons_released & mask) != 0;
}

static inline uint32_t of_input_state(int player, of_input_state_t *state) {
    return (uint32_t)__of_syscall2(OF_SYS_INPUT_GET_STATE, player, (long)state);
}

static inline void of_input_set_deadzone(int16_t deadzone) {
    __of_syscall1(OF_SYS_INPUT_SET_DEADZONE, deadzone);
}

#else /* OF_PC */

void     of_input_poll(void);
int      of_btn(uint32_t mask);
int      of_btn_pressed(uint32_t mask);
int      of_btn_released(uint32_t mask);
int      of_btn_p2(uint32_t mask);
int      of_btn_pressed_p2(uint32_t mask);
int      of_btn_released_p2(uint32_t mask);
uint32_t of_input_state(int player, of_input_state_t *state);
void     of_input_set_deadzone(int16_t deadzone);

#endif /* OF_PC */

#ifdef __cplusplus
}
#endif

#endif /* OF_INPUT_H */
