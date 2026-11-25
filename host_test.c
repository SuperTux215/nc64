/* Host-side self test for the emulator core. Uses a tiny hand-assembled fake
 * KERNAL so no Commodore ROMs are needed.
 *   gcc -O2 -Wall -o host_test host_test.c c64.c && ./host_test */
#include <stdio.h>
#include <string.h>
#include "c64.h"

static uint8_t kern[8192], bas[8192], chr[4096];
static int fails;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); fails++; } else printf("ok:   %s\n", msg); } while (0)

static void emit(int addr, const uint8_t *b, int n) { memcpy(kern + (addr - 0xE000), b, n); }

int main(void) {
  static const uint8_t main_code[] = {
    0x78,                         /* SEI */
    0xA2,0x00,                    /* LDX #0 */
    0x8A,                         /* TXA */
    0x9D,0x00,0x04,               /* STA $0400,X */
    0xA9,0x01,                    /* LDA #1 */
    0x9D,0x00,0xD8,               /* STA $D800,X */
    0xE8,                         /* INX */
    0xD0,0xF4,                    /* BNE $E003 */
    0xA9,0x06, 0x8D,0x20,0xD0,    /* border = blue */
    0xA9,0x00, 0x8D,0x21,0xD0,    /* background = black */
    0xA9,0x1B, 0x8D,0x11,0xD0,    /* D011: DEN, 25 rows, yscroll 3 */
    0xA9,0x14, 0x8D,0x18,0xD0,    /* D018: screen $0400, chars $1000 */
    0xA9,0xC8, 0x8D,0x16,0xD0,    /* D016: 40 cols */
    0xA9,0x00, 0x8D,0x04,0xDC,    /* CIA1 TA latch lo */
    0xA9,0x4C, 0x8D,0x05,0xDC,    /* CIA1 TA latch hi ($4C00 = 19456 cycles) */
    0xA9,0x81, 0x8D,0x0D,0xDC,    /* enable TA irq */
    0xA9,0x11, 0x8D,0x0E,0xDC,    /* start, force load */
    0x58,                         /* CLI */
    0x4C,0x3D,0xE0                /* JMP * */
  };
  static const uint8_t irq_code[] = { 0xEE,0x10,0x03, 0xAD,0x0D,0xDC, 0x40 };
  emit(0xE000, main_code, sizeof main_code);
  emit(0xE100, irq_code, sizeof irq_code);
  kern[0x1FFC] = 0x00; kern[0x1FFD] = 0xE0;
  kern[0x1FFE] = 0x00; kern[0x1FFF] = 0xE1;
  for (int c = 0; c < 256; c++) for (int r = 0; r < 8; r++) chr[c * 8 + r] = (c == 0) ? 0 : (r & 1 ? 0xAA : 0x55);
  c64_init(kern, bas, chr);

  /* --- CPU test, runs from RAM --- */
  static const uint8_t t[] = {
    0xF8, 0x18, 0xA9,0x19, 0x69,0x28, 0x8D,0x00,0x03,          /* SED CLC LDA #$19 ADC #$28 STA $0300 */
    0x38, 0xA9,0x50, 0xE9,0x30, 0x8D,0x01,0x03, 0xD8,          /* SEC LDA #$50 SBC #$30 STA $0301 CLD */
    0xA2,0x05, 0xCA, 0xD0,0xFD, 0x8E,0x02,0x03,                /* LDX #5; DEX; BNE; STX $0302 */
    0x20,0x60,0x02, 0x8D,0x03,0x03,                            /* JSR $0230; STA $0303 */
    0xA9,0xFF, 0x18, 0x69,0x01, 0x08, 0x68, 0x8D,0x04,0x03,    /* ADC overflow, PHP PLA STA $0304 */
    0xA9,0x80, 0x0A, 0x8D,0x05,0x03,                           /* ASL A -> 0; STA $0305 */
    0xA9,0x37, 0x85,0x10, 0xE6,0x10, 0xA5,0x10, 0x8D,0x06,0x03,/* INC zp -> $38 */
    0x4C,0x80,0x02
  };
  for (unsigned i = 0; i < sizeof t; i++) c64_poke(0x200 + i, t[i]);
  c64_poke(0x260, 0xA9); c64_poke(0x261, 0x42); c64_poke(0x262, 0x60);
  c64_dbg_exec(0x200, 0x280, 1000);
  CHECK(c64_peek(0x300) == 0x47, "decimal ADC $19+$28 = $47");
  CHECK(c64_peek(0x301) == 0x20, "decimal SBC $50-$30 = $20");
  CHECK(c64_peek(0x302) == 0x00, "DEX/BNE loop");
  CHECK(c64_peek(0x303) == 0x42, "JSR/RTS");
  CHECK(c64_peek(0x304) == 0x37, "ADC wrap sets C and Z (P=$37)");
  CHECK(c64_peek(0x305) == 0x00, "ASL A");
  CHECK(c64_peek(0x306) == 0x38, "INC zp");

  /* --- machine test: video + timer IRQ --- */
  c64_reset();
  for (int i = 0; i < 4; i++) c64_run_frame();
  int irqs = c64_peek(0x310);
  printf("timer IRQs after 4 frames: %d\n", irqs);
  CHECK(irqs >= 3 && irqs <= 4, "CIA1 timer A interrupts at ~50 Hz");
  CHECK(c64_peek(0x401) == 1 && c64_peek(0x4FF) == 0xFF, "screen RAM filled by program");
  CHECK(c64_fb[0] == c64_fb[319 * 1] && c64_fb[0] != 0, "top border drawn");
  int nonzero = 0;
  for (int y = 20; y < 220; y++) for (int x = 0; x < 320; x++) if (c64_fb[y * 320 + x] == 0xFFFF) nonzero++;
  printf("white pixels in text area: %d\n", nonzero);
  CHECK(nonzero > 1000, "text mode renders foreground pixels");

  /* sprite test */
  for (int i = 0; i < 63; i++) c64_poke(0x340 + i, 0xFF);
  c64_poke(0x7F8, 0x0D);
  c64_poke(0xD000, 24 + 100); c64_poke(0xD001, 50 + 60);
  c64_poke(0xD027, 2);
  c64_poke(0xD015, 1);
  c64_run_frame(); c64_run_frame();
  uint16_t px = c64_fb[(20 + 60 + 2) * 320 + 100 + 2];
  printf("sprite pixel = %04X (expected %04X)\n", px, 0x68372B ? (((0x68372B >> 8) & 0xF800) | ((0x68372B >> 5) & 0x7E0) | ((0x68372B >> 3) & 0x1F)) : 0);
  CHECK(px == (uint16_t)((((0x68372B) >> 8) & 0xF800) | (((0x68372B) >> 5) & 0x07E0) | (((0x68372B) >> 3) & 0x001F)), "sprite 0 drawn at expected position");

  /* ASCII dump of a slice of the frame, one char per 4 px */
  for (int y = 16; y < 40; y += 2) {
    for (int x = 0; x < 160; x += 2) {
      uint16_t p = c64_fb[y * 320 + x];
      putchar(p == 0xFFFF ? '#' : p == c64_fb[0] ? '.' : p == 0 ? ' ' : '+');
    }
    putchar('\n');
  }
  printf(fails ? "\n%d FAILURES\n" : "\nall tests passed\n", fails);
  return fails != 0;
}
