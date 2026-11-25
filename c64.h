/* Portable Commodore 64 emulator core (PAL, 6510 + VIC-II + 2x CIA).
 * No SID audio. Framebuffer is 320x240 RGB565: 200 visible lines plus
 * 20 lines of border above and below (side borders are not drawn). */
#ifndef C64_EMU_H
#define C64_EMU_H
#include <stdint.h>

#define C64_FB_W 320
#define C64_FB_H 240

extern uint16_t c64_fb[C64_FB_W * C64_FB_H];

/* Keyboard matrix index = column*8 + row (column = CIA1 port A bit,
 * row = CIA1 port B bit). */
#define CK(c, r) ((c) * 8 + (r))
enum {
  K_DEL = CK(0,0), K_RET = CK(0,1), K_CRIGHT = CK(0,2), K_F7 = CK(0,3),
  K_F1 = CK(0,4), K_F3 = CK(0,5), K_F5 = CK(0,6), K_CDOWN = CK(0,7),
  K_3 = CK(1,0), K_W = CK(1,1), K_A = CK(1,2), K_4 = CK(1,3),
  K_Z = CK(1,4), K_S = CK(1,5), K_E = CK(1,6), K_LSHIFT = CK(1,7),
  K_5 = CK(2,0), K_R = CK(2,1), K_D = CK(2,2), K_6 = CK(2,3),
  K_C = CK(2,4), K_F = CK(2,5), K_T = CK(2,6), K_X = CK(2,7),
  K_7 = CK(3,0), K_Y = CK(3,1), K_G = CK(3,2), K_8 = CK(3,3),
  K_B = CK(3,4), K_H = CK(3,5), K_U = CK(3,6), K_V = CK(3,7),
  K_9 = CK(4,0), K_I = CK(4,1), K_J = CK(4,2), K_0 = CK(4,3),
  K_M = CK(4,4), K_K = CK(4,5), K_O = CK(4,6), K_N = CK(4,7),
  K_PLUS = CK(5,0), K_P = CK(5,1), K_L = CK(5,2), K_MINUS = CK(5,3),
  K_DOT = CK(5,4), K_COLON = CK(5,5), K_AT = CK(5,6), K_COMMA = CK(5,7),
  K_POUND = CK(6,0), K_STAR = CK(6,1), K_SEMI = CK(6,2), K_HOME = CK(6,3),
  K_RSHIFT = CK(6,4), K_EQ = CK(6,5), K_UPARROW = CK(6,6), K_SLASH = CK(6,7),
  K_1 = CK(7,0), K_LARROW = CK(7,1), K_CTRL = CK(7,2), K_2 = CK(7,3),
  K_SPACE = CK(7,4), K_CBM = CK(7,5), K_Q = CK(7,6), K_STOP = CK(7,7)
};

/* ROM images: kernal 8192 bytes, basic 8192 bytes, chargen 4096 bytes.
 * The data is copied. The ROMs are NOT included with this project. */
void c64_init(const uint8_t *kernal, const uint8_t *basic, const uint8_t *chargen);
void c64_reset(void);
void c64_run_frame(void);                 /* one PAL frame, 312 lines */

void c64_key(int k, int down);
void c64_joy(int port, int up, int down, int left, int right, int fire); /* port 1 or 2 */
void c64_restore(void);                   /* RESTORE key (NMI) */

/* Reset, then once BASIC is at READY copy the PRG in and RUN / SYS it. */
void c64_autostart_prg(const uint8_t *prg, int len);

/* D64 disk image (data must stay valid). LOAD"name",8[,1] and LOAD"$",8
 * are served by a KERNAL trap; fast loaders, SAVE and sequential files are not supported. */
void c64_mount_d64(const uint8_t *d64, int len);
int  c64_d64_count(void);
int  c64_d64_entry(int i, char *name17, int *blocks);
int  c64_d64_read(int i, uint8_t *buf, int max);   /* includes 2-byte load address */

const uint8_t *c64_chargen(void);

/* debug / test helpers */
void    c64_poke(uint16_t a, uint8_t v);
uint8_t c64_peek(uint16_t a);
int     c64_dbg_exec(uint16_t start, uint16_t stop, int maxsteps);
uint8_t c64_dbg_reg(char r);              /* 'a','x','y','s','p' */
#endif
