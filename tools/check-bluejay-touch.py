#!/usr/bin/env python3
"""Exercise the actual Bluejay ACPM transport, FTM5 decoder, and GPIO ownership.

Uses host MMIO/timing boundaries with AddressSanitizer and UBSan; leaves
compiled harness and log under research/bluejay-touch/. Does not access USB.
"""
from pathlib import Path
import re
import subprocess

import sys
repo = Path(__file__).resolve().parents[1]
if len(sys.argv) != 2:
    raise SystemExit('usage: check-bluejay-touch.py PATCHED_XNU_DIRECTORY')
source = (Path(sys.argv[1]) / 'bsd/dev/bluejay_touch.c').read_text()
source = source[source.index('#define BT_GPP_PA'):source.rindex('#endif')]
source = re.sub(r'\t__asm__ volatile\([^\n]*\);\n', '', source)
# Only platform boundaries are stubbed: device reads/writes and kernel timing.
source = source.replace('uint32_t value = *(volatile uint32_t *)(base + off);',
                        'uint32_t value = *(volatile uint32_t *)(base + off);\n\thost_after_read(base, off, &value);')
source = source.replace('*(volatile uint32_t *)(base + off) = value;',
                        '*(volatile uint32_t *)(base + off) = value;\n\thost_after_write(base, off, value);')

prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#define __unused __attribute__((unused))
#define NSEC_PER_MSEC 1000000
#define THREAD_UNINT 0
#define THREAD_CONTINUE_NULL 0
#define KERN_SUCCESS 0
typedef uintptr_t vm_offset_t;
typedef size_t vm_size_t;
typedef int kern_return_t;
typedef int wait_result_t;
typedef void *thread_t;
static vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
static void host_after_read(vm_offset_t, uint32_t, uint32_t *);
static void host_after_write(vm_offset_t, uint32_t, uint32_t);
static void sweet_hid_touch(int, int, int, int);
static int sweet_hid_touch_ready(void) { return 1; }
static void clock_interval_to_deadline(unsigned ms, unsigned scale, uint64_t *d) { *d = (uint64_t)ms * scale; }
static int assert_wait_deadline(void *c, int t, uint64_t d) { (void)c; (void)t; (void)d; return 0; }
static void thread_block(void *c) { (void)c; }
static void delay(int us) { (void)us; }
static thread_t current_thread(void) { return NULL; }
static int thread_terminate(thread_t t) { (void)t; return 0; }
static int kernel_thread_start(void (*f)(void *, int), void *a, thread_t *t) { (void)f; (void)a; *t = NULL; return 1; }
static void thread_deallocate(thread_t t) { (void)t; }
static int PE_parse_boot_argn(const char *k, void *v, int n) { (void)k; (void)v; (void)n; return 0; }
'''

tests = r'''
static uint32_t sram[BT_SRAM_SIZE / 4], mbox[0x1000 / 4];
static uint32_t gpp[0x1000 / 4], irqmem[0x1000 / 4], aoc[0x1000 / 4];
static unsigned sends, acks, hid_count, test_count;
static int response_mode, status_code;
static bool reply_echo, inject_stale;
static uint8_t pmic[2];
struct hid_record { int id, down, x, y; };
static struct hid_record hid[64];

static vm_offset_t ml_io_map(vm_offset_t pa, vm_size_t sz) {
    (void)sz;
    if (pa == BT_SRAM_PA) return (uintptr_t)sram;
    if (pa == BT_MBOX_PA) return (uintptr_t)mbox;
    if (pa == BT_GPP_PA) return (uintptr_t)gpp;
    if (pa == BT_IRQ_PA) return (uintptr_t)irqmem;
    if (pa == BT_AOC_PA) return (uintptr_t)aoc;
    assert(false); return 0;
}
static void host_after_read(vm_offset_t base, uint32_t off, uint32_t *v) {
    (void)v;
    if (base == (uintptr_t)sram) assert(off + 4 <= sizeof(sram));
}
static void queue_reply(uint32_t word0, uint32_t word1) {
    uint32_t front = sram[bt.ch.tx_front / 4];
    uint32_t next = (front + 1) % bt.ch.qlen;
    assert(next != sram[bt.ch.tx_rear / 4]);
    uint32_t off = bt.ch.tx_base + front * bt.ch.mlen;
    sram[off / 4] = word0;
    sram[off / 4 + 1] = word1;
    sram[bt.ch.tx_front / 4] = next;
}
static void host_after_write(vm_offset_t base, uint32_t off, uint32_t value) {
    if (base == (uintptr_t)sram) {
        assert(off + 4 <= sizeof(sram));
        return;
    }
    if (base != (uintptr_t)mbox) return;
    if (off == BT_MBOX_ACK) { assert(value == 4); acks++; return; }
    if (off != BT_MBOX_SEND) return;
    assert(value == 4);
    sends++;
    uint32_t rear = sram[bt.ch.rx_rear / 4];
    uint32_t front = sram[bt.ch.rx_front / 4];
    assert(front == (rear + 1) % bt.ch.qlen); /* TX published before doorbell */
    uint32_t slot = bt.ch.rx_base + rear * bt.ch.mlen;
    uint32_t request = sram[slot / 4], args = sram[slot / 4 + 1];
    unsigned seq = (request >> 16) & 63, reg = request & 255, func = args & 255;
    assert(seq >= 1 && seq <= 63 && (request & 0xff00) == 0x100);
    assert(reg == 0x5c || reg == 0x5d);
    assert(sram[slot / 4 + 2] == 0 && sram[slot / 4 + 3] == 0);
    sram[bt.ch.rx_rear / 4] = front;
    if (response_mode == 0) return;
    if (func == 1 && !status_code) pmic[reg - 0x5c] = (args >> 8) & 255;
    if (inject_stale) queue_reply(((seq % 63) + 1) << 16, 0x9900);
    if (response_mode == 2) return; /* Only the stale response arrives. */
    uint32_t header = reply_echo ? request : seq << 16;
    queue_reply(header | (1u << 30), ((uint32_t)status_code << 24) | ((uint32_t)pmic[reg - 0x5c] << 8));
}
static void sweet_hid_touch(int id, int down, int x, int y) {
    assert(hid_count < 64);
    hid[hid_count++] = (struct hid_record){id, down, x, y};
}
static void setup(void) {
    memset(&bt, 0, sizeof(bt));
    memset(sram, 0, sizeof(sram)); memset(mbox, 0, sizeof(mbox));
    memset(gpp, 0, sizeof(gpp)); memset(irqmem, 0, sizeof(irqmem)); memset(aoc, 0, sizeof(aoc));
    sends = acks = hid_count = 0; response_mode = 1; status_code = 0; reply_echo = true; inject_stale = false;
    pmic[0] = 0xec; pmic[1] = 0xfc;
    struct bt_acpm_header h = { .channels = 0xb000, .num_channels = 3, .ap_channels = 3 };
    memcpy((char *)sram + BT_INITDATA, &h, sizeof(h));
    struct bt_acpm_channel c = {
        .id = 2, .type = 1,
        .rx_rear = 0x12000, .rx_front = 0x12004, .rx_base = 0x13000,
        .tx_rear = 0x12008, .tx_front = 0x1200c, .tx_base = 0x14000,
        .qlen = 4, .mlen = 16, .ap_poll = 1,
    };
    /* PMIC descriptor deliberately at index1, not the Windows index2. */
    memcpy((char *)sram + h.channels + sizeof(c), &c, sizeof(c));
}
static void packet(uint8_t *e, uint8_t event, unsigned id, unsigned tool, unsigned x, unsigned y) {
    memset(e, 0, 8); e[0] = event; e[1] = (id << 4) | tool;
    e[2] = x & 255; e[3] = (x >> 8) | ((y & 15) << 4); e[4] = y >> 4;
}
static void check(bool v) { assert(v); test_count++; }
int main(void) {
    setup(); check(bt_acpm_open() == 0); check(bt.ch.id == 2 && bt.ch.qlen == 4);
    check((mbox[BT_MBOX_MASK / 4] & 4) != 0);
    uint8_t value = 0;
    check(bt_acpm_xfer(0, 0x5c, 0, &value) == 0 && value == 0xec && sends == 1);
    check(sram[bt.ch.tx_front / 4] == sram[bt.ch.tx_rear / 4] && acks != 0);
    inject_stale = true; reply_echo = false;
    check(bt_acpm_xfer(0, 0x5d, 0, &value) == 0 && value == 0xfc && bt.skipped == 1);
    status_code = 7; inject_stale = false; value = 0x77;
    check(bt_acpm_xfer(0, 0x5c, 0, &value) == EIO && value == 0x77);
    bt_acpm_close(); check((mbox[BT_MBOX_MASK / 4] & 4) == 0);

    setup(); check(bt_acpm_open() == 0); response_mode = 0;
    check(bt_acpm_xfer(0, 0x5c, 0, &value) == ETIMEDOUT && sends == 1);
    setup(); check(bt_acpm_open() == 0); response_mode = 2; inject_stale = true;
    check(bt_acpm_xfer(0, 0x5c, 0, &value) == ETIMEDOUT && bt.skipped == 1);

    setup(); check(bt_acpm_open() == 0);
    sram[bt.ch.rx_front / 4] = 3; sram[bt.ch.rx_rear / 4] = 0;
    check(bt_acpm_xfer(0, 0x5c, 0, &value) == EBUSY && sends == 0);
    setup(); check(bt_acpm_open() == 0);
    sram[bt.ch.rx_front / 4] = 2; sram[bt.ch.rx_rear / 4] = 1;
    check(bt_acpm_xfer(0, 0x5c, 0, &value) == EBUSY && sends == 0); /* reserve2 */
    setup(); check(bt_acpm_open() == 0); sram[bt.ch.rx_front / 4] = 4;
    check(bt_acpm_xfer(0, 0x5c, 0, &value) == EPROTO && sends == 0);
    setup(); check(bt_acpm_open() == 0); bt.seq = 63;
    check(bt_acpm_xfer(0, 0x5c, 0, &value) == 0 && bt.seq == 1);

    setup(); pmic[0] = pmic[1] = 0;
    check(bt_power() == 0 && pmic[0] == 0xec && pmic[1] == 0xfc && sends == 6);
    setup(); check(bt_power() == 0 && sends == 4); /* rails already right */

    const unsigned bad_offsets[] = {0, 1, 0x40000, 0x3fffc, 0xffffffff};
    for (unsigned i = 0; i < sizeof(bad_offsets)/sizeof(bad_offsets[0]); i++) {
        setup(); struct bt_acpm_channel *c = (void *)((char *)sram + 0xb000 + 72);
        c->rx_base = bad_offsets[i]; check(bt_acpm_open() == EPROTO && sends == 0);
    }
    setup(); struct bt_acpm_channel *c = (void *)((char *)sram + 0xb000 + 72);
    c->tx_base = c->rx_base; check(bt_acpm_open() == EPROTO);
    setup(); c = (void *)((char *)sram + 0xb000 + 72); c->rx_rear = c->rx_front;
    check(bt_acpm_open() == EPROTO);
    setup(); c = (void *)((char *)sram + 0xb000 + 72); c->rx_front = 0xb000;
    check(bt_acpm_open() == EPROTO);
    setup(); c = (void *)((char *)sram + 0xb000 + 72); c->qlen = 0xffffffff;
    check(bt_acpm_open() == EPROTO);
    setup(); c = (void *)((char *)sram + 0xb000 + 72); c->mlen = 17;
    check(bt_acpm_open() == EPROTO);
    setup(); c = (void *)((char *)sram + 0xb000 + 72); c->field = 1u << 31;
    check(bt_acpm_open() == EPROTO);
    setup(); memcpy((char *)sram + 0xb000 + 144, (char *)sram + 0xb000 + 72, 72);
    check(bt_acpm_open() == EPROTO);

    setup(); uint8_t e[8];
    packet(e, 0x13, 3, 1, 1079, 2399); check(bt_event(e) == 0);
    check(hid_count == 1 && hid[0].id == 3 && hid[0].down == 1 && hid[0].x == 1079 && hid[0].y == 2399);
    packet(e, 0x23 | 8, 3, 1, 501, 1234); check(bt_event(e) == 0);
    check(hid[1].x == 501 && hid[1].y == 1234); /* event flags masked */
    packet(e, 0x13, 7, 2, 321, 678); check(bt_event(e) == 0 && bt.active == ((1u << 3)|(1u << 7)));
    packet(e, 0x33, 3, 1, 0, 0); check(bt_event(e) == 0);
    check(hid[3].id == 3 && hid[3].down == 0 && hid[3].x == 501 && hid[3].y == 1234 && bt.active == (1u << 7));
    packet(e, 0x13, 10, 1, 1, 2); check(bt_event(e) == EPROTO && hid_count == 4);
    packet(e, 0x13, 2, 1, 4095, 4095); check(bt_event(e) == EPROTO && hid_count == 4);
    packet(e, 0x13, 7, 4, 1, 2); check(bt_event(e) == 0 && bt.active == 0 && hid[4].down == 0); /* palm */
    packet(e, 0x13, 2, 1, 123, 456); check(bt_event(e) == 0);
    e[0] = 3; check(bt_event(e) == EAGAIN && bt.active == 0 && hid[6].down == 0); /* reset */
    packet(e, 0x13, 1, 1, 123, 456); check(bt_event(e) == 0);
    e[0] = 0xf3; check(bt_event(e) == EAGAIN && bt.active == 0 && hid[8].down == 0); /* error */
    e[0] = 0; check(bt_event(e) == 0); e[0] = 0xff; check(bt_event(e) == EPROTO);

    setup(); gpp[(BT_SPI_BANK + BT_CON)/4] = 0xabcd5678;
    gpp[(BT_SPI_BANK + BT_PUD)/4] = 0x12345678;
    gpp[(BT_RESET_BANK + BT_CON)/4] = 0x24680111;
    gpp[(BT_RESET_BANK + BT_DAT)/4] = 0x1f;
    check(bt_take_pins() == 0);
    check(gpp[(BT_SPI_BANK + BT_CON)/4] == 0xabcd1011 && gpp[(BT_SPI_BANK + BT_PUD)/4] == 0x12343300);
    bt_restore_pins(); check(gpp[(BT_SPI_BANK + BT_CON)/4] == 0xabcd5678 && gpp[(BT_RESET_BANK + BT_DAT)/4] == 0x1f);
    setup(); aoc[(BT_AOC_BANK + BT_DAT)/4] = 1u << 6;
    check(bt_take_pins() == ETIMEDOUT && gpp[(BT_SPI_BANK + BT_CON)/4] == 0);
    bt_restore_pins(); check(gpp[(BT_RESET_BANK + BT_CON)/4] == 0);
    printf("BLUEJAY_TOUCH_HOST_PASS checks=%u\n", test_count);
    return 0;
}
'''
target = repo / 'research/bluejay-touch/host-check.c'
target.write_text(prefix + source + tests)
binary = target.with_suffix('')
subprocess.run(['/usr/bin/clang-21', '-std=c11', '-O1', '-g', '-fsanitize=address,undefined',
                '-fno-omit-frame-pointer', '-Wno-format', str(target), '-o', str(binary)], check=True)
result = subprocess.run([str(binary)], capture_output=True, text=True, check=False)
log = repo / 'research/bluejay-touch/host-check.log'
log.write_text(result.stdout + result.stderr)
print('\n'.join(result.stdout.splitlines()[-8:]))
if result.returncode:
    print(result.stderr)
    raise SystemExit(result.returncode)
print('Actual driver ACPM/event/GPIO checks passed with AddressSanitizer + UBSan')
