#include "defines.h"
#include "keypad.h"
#include "emu.h"
#include "schedule.h"
#include "interrupt.h"
#include "control.h"
#include "asic.h"

/* Global KEYPAD state */
keypad_state_t keypad;

void keypad_intrpt_check() {
    intrpt_set(INT_KEYPAD, (keypad.status & keypad.enable) | (keypad.gpio_status & keypad.gpio_enable));
}

void EMSCRIPTEN_KEEPALIVE keypad_key_event(unsigned int row, unsigned int col, bool press) {
    if (row == 2 && col == 0) {
        intrpt_set(INT_ON, press);
        if (press && calc_is_off()) {
            if (asic.resetOnWake) {
                cpuEvents |= EVENT_RESET;
            }
            control.readBatteryStatus = ~1;
            intrpt_pulse(INT_WAKE);
        }
    } else {
        if (press) {
            keypad.keyMap[row] |= 1 << col;
            if (keypad.mode == 1) {
                keypad.status |= 4;
                keypad_intrpt_check();
            }
        } else {
            keypad.keyMap[row] &= ~(1 << col);
            keypad_intrpt_check();
        }
    }
}

static uint8_t keypad_read(const uint16_t pio, bool peek) {
    uint16_t index = (pio >> 2) & 0x7F;
    uint8_t bit_offset = (pio & 3) << 3;
    uint8_t value = 0;
    (void)peek;

    switch(index) {
        case 0x00:
            value = read8(keypad.control, bit_offset);
            break;
        case 0x01:
            value = read8(keypad.size, bit_offset);
            break;
        case 0x02:
            value = read8(keypad.status & keypad.enable, bit_offset);
            break;
        case 0x03:
            value = read8(keypad.enable, bit_offset);
            break;
        case 0x04: case 0x05: case 0x06: case 0x07:
        case 0x08: case 0x09: case 0x0A: case 0x0B:
            value = read8(keypad.data[(pio - 0x10) >> 1 & 15], pio << 3 & 8);
            break;
        case 0x10:
            value = read8(keypad.gpio_enable, bit_offset);
            break;
        case 0x11:
            value = read8(keypad.gpio_status, bit_offset);
            break;
        default:
            break;
    }

    /* return 0x00 if unimplemented or not in range */
    return value;
}

/* Scan next row of keypad, if scanning is enabled */
static void keypad_scan_event(int index) {
    uint16_t row;

    if (keypad.current_row >= sizeof(keypad.data) / sizeof(keypad.data[0])) {
        return; /* too many keypad rows */
    }

    /* scan each data row */
    row = keypad.keyMap[keypad.current_row];
    row &= (1 << keypad.cols) - 1;

    /* if mode 3 or 2, generate data change interrupt */
    if (keypad.data[keypad.current_row] != row) {
        keypad.status |= 2;
        keypad.data[keypad.current_row] = row;
    }

    if (keypad.current_row++ < keypad.rows) {  /* scan the next row */
        event_repeat(index, keypad.row_wait);
    } else {  /* finished scanning the keypad */
        keypad.current_row = 0;
        keypad.status |= 1;
        if (keypad.mode & 1) { /* are we in mode 1 or 3 */
            event_repeat(index, keypad.scan_wait + keypad.row_wait);
        } else {
            /* If in single scan mode, go to idle mode */
            keypad.mode = 0;
        }
    }
    keypad_intrpt_check();
}

static void keypad_write(const uint16_t pio, const uint8_t byte, bool poke) {
    int row;
    uint16_t index = (pio >> 2) & 0x7F;
    uint8_t bit_offset = (pio & 3) << 3;

    switch (index) {
        case 0x00:
            write8(keypad.control,bit_offset,byte);
            if (keypad.mode & 2) {
                keypad.current_row = 0;
                event_set(SCHED_KEYPAD, keypad.scan_wait + keypad.row_wait);
            } else {
                event_clear(SCHED_KEYPAD);
                if (keypad.mode == 1) {
                    for (row = 0; row < keypad.rows; row++) {
                        if (keypad.keyMap[row] & ((1 << keypad.cols) - 1)) {
                            keypad.status |= 4;
                            keypad_intrpt_check();
                            break;
                        }
                    }
                }
            }
            break;
        case 0x01:
            write8(keypad.size, bit_offset, byte);
            break;
        case 0x02:
            write8(keypad.status, bit_offset, keypad.status & ~byte);
            if (keypad.mode == 1) {
                for (row = 0; row < keypad.rows; row++) {
                    if (keypad.keyMap[row] & ((1 << keypad.cols) - 1)) {
                        keypad.status |= 4;
                        break;
                    }
                }
            }
            keypad_intrpt_check();
            break;
        case 0x03:
            write8(keypad.enable, bit_offset, byte & 7);
            keypad_intrpt_check();
            break;
        case 0x04: case 0x05: case 0x06: case 0x07:
        case 0x08: case 0x09: case 0x0A: case 0x0B:
            if (poke) {
                write8(keypad.data[(pio - 0x10) >> 1 & 15], pio << 3 & 8, byte);
                write8(keypad.keyMap[pio >> 1 & 15], pio << 3 & 8, byte);
            }
            break;
        case 0x10:
            write8(keypad.gpio_enable, bit_offset, byte);
            keypad_intrpt_check();
            break;
        case 0x11:
            write8(keypad.gpio_status, bit_offset, keypad.gpio_status & ~byte);
            keypad_intrpt_check();
            break;
        default:
            break;  /* Escape write sequence if unimplemented */
    }
}

void keypad_reset() {
    unsigned int i;

    keypad.current_row = 0;
    for(i=0; i<sizeof(keypad.data) / sizeof(keypad.data[0]); i++) {
        keypad.data[i] = keypad.keyMap[i] = 0;
    }

    sched.items[SCHED_KEYPAD].clock = CLOCK_APB;
    sched.items[SCHED_KEYPAD].second = -1;
    sched.items[SCHED_KEYPAD].proc = keypad_scan_event;

    gui_console_printf("[CEmu] Keypad reset.\n");
}

static const eZ80portrange_t device = {
    .read  = keypad_read,
    .write = keypad_write
};

eZ80portrange_t init_keypad(void) {
    gui_console_printf("[CEmu] Initialized Keypad...\n");
    return device;
}

bool keypad_save(emu_image *s) {
    s->keypad = keypad;
    return true;
}

bool keypad_restore(const emu_image *s) {
    keypad = s->keypad;
    return true;
}
