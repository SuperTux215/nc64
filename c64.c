/* Portable C64 emulator core. See c64.h. */
#include "c64.h"
#include <string.h>

uint16_t c64_fb[C64_FB_W * C64_FB_H];

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */
static uint8_t ram[65536];
static uint8_t cram[1024];
static uint8_t kernal[8192], basic[8192], chargen[4096];

static uint8_t cpu_ddr = 0x2F, cpu_dat = 0x37;
static int bk_basic, bk_kernal, bk_io, bk_char;

static uint8_t vic[64];
static uint8_t sid[32];
static uint8_t vic_irq;           /* bits 0..3 */
static int raster, frames;
static uint16_t vcbase;
static int rc, ds, den_latch;
static uint8_t vm_chr[40], vm_col[40];
static uint16_t vbank;

typedef struct {
  uint8_t pra, prb, ddra, ddrb, cra, crb, icr_mask, icr_data;
  uint16_t ta, tb, latch_a, latch_b;
  uint8_t tod[4];
  int todcnt;
} CIA;
static CIA cia1, cia2;

static uint8_t kbd[8];
static uint8_t joy1 = 0xFF, joy2 = 0xFF;

static uint16_t pc;
static uint8_t ra, rx, ry, sp;
static uint8_t fC, fZ, fI, fD, fV, fN;
static int irq_line, nmi_prev, nmi_pending;
static int carry;

/* autostart / disk */
static uint8_t auto_buf[65536 + 2];
static int auto_len, auto_pending;
static const uint8_t *d64;
static int d64len;

#define RGB565(rgb) ((uint16_t)((((rgb) >> 8) & 0xF800) | (((rgb) >> 5) & 0x07E0) | (((rgb) >> 3) & 0x001F)))
static const uint16_t pal[16] = {
  RGB565(0x000000), RGB565(0xFFFFFF), RGB565(0x68372B), RGB565(0x70A4B2),
  RGB565(0x6F3D86), RGB565(0x588D43), RGB565(0x352879), RGB565(0xB8C76F),
  RGB565(0x6F4F25), RGB565(0x433900), RGB565(0x9A6759), RGB565(0x444444),
  RGB565(0x6C6C6C), RGB565(0x9AD284), RGB565(0x6C5EB5), RGB565(0x959595)
};

/* ------------------------------------------------------------------ */
/* Interrupt lines                                                     */
/* ------------------------------------------------------------------ */
static void upd_irq(void) {
  irq_line = ((cia1.icr_data & cia1.icr_mask & 0x1F) != 0) ||
             ((vic_irq & vic[0x1A] & 0x0F) != 0);
  int nmi = (cia2.icr_data & cia2.icr_mask & 0x1F) != 0;
  if (nmi && !nmi_prev) nmi_pending = 1;
  nmi_prev = nmi;
}

/* ------------------------------------------------------------------ */
/* Memory                                                              */
/* ------------------------------------------------------------------ */
static void upd_bank(void) {
  uint8_t p = (cpu_dat | ~cpu_ddr) & 7;
  int lo = p & 1, hi = (p >> 1) & 1, ch = (p >> 2) & 1;
  bk_basic = lo && hi;
  bk_kernal = hi;
  if (!lo && !hi) { bk_io = 0; bk_char = 0; }
  else { bk_io = ch; bk_char = !ch; }
}

static uint32_t rng = 0x12345678;
static uint8_t rnd8(void) {
  rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
  return (uint8_t)(rng >> 8);
}

static uint8_t vic_rd(int r) {
  uint8_t v;
  if (r >= 0x2F) return 0xFF;
  switch (r) {
    case 0x11: return (vic[0x11] & 0x7F) | ((raster & 0x100) >> 1);
    case 0x12: return raster & 0xFF;
    case 0x16: return vic[r] | 0xC0;
    case 0x18: return vic[r] | 0x01;
    case 0x19: return vic_irq | 0x70 | ((vic_irq & vic[0x1A] & 0x0F) ? 0x80 : 0);
    case 0x1A: return vic[r] | 0xF0;
    case 0x1E: case 0x1F: v = vic[r]; vic[r] = 0; return v;
    default: break;
  }
  if (r >= 0x20) return vic[r] | 0xF0;
  return vic[r];
}

static void vic_wr(int r, uint8_t v) {
  if (r >= 0x2F) return;
  switch (r) {
    case 0x19: vic_irq &= ~(v & 0x0F); upd_irq(); return;
    case 0x1A: vic[r] = v & 0x0F; upd_irq(); return;
    case 0x1E: case 0x1F: return;
    default: vic[r] = v;
  }
}

static uint8_t kbd_scan(uint8_t cols) {
  uint8_t rows = 0xFF;
  for (int c = 0; c < 8; c++)
    if (!(cols & (1 << c))) rows &= kbd[c];
  return rows;
}

static uint8_t cia_rd(CIA *c, int r, int which) {
  uint8_t v;
  switch (r) {
    case 0:
      if (which == 1) return (c->pra | ~c->ddra) & joy2;
      return (c->pra | ~c->ddra);
    case 1:
      if (which == 1) {
        uint8_t cols = (c->pra | ~c->ddra) & joy2;
        uint8_t rows = kbd_scan(cols);
        v = (c->prb & c->ddrb) | (rows & ~c->ddrb);
        return v & joy1;
      }
      return (c->prb | ~c->ddrb);
    case 2: return c->ddra;
    case 3: return c->ddrb;
    case 4: return c->ta & 0xFF;
    case 5: return c->ta >> 8;
    case 6: return c->tb & 0xFF;
    case 7: return c->tb >> 8;
    case 8: case 9: case 10: case 11: return c->tod[r - 8];
    case 12: return 0;
    case 13:
      v = c->icr_data;
      if (v & c->icr_mask & 0x1F) v |= 0x80;
      c->icr_data = 0;
      upd_irq();
      return v;
    case 14: return c->cra;
    default: return c->crb;
  }
}

static void cia_wr(CIA *c, int r, uint8_t v) {
  switch (r) {
    case 0: c->pra = v; break;
    case 1: c->prb = v; break;
    case 2: c->ddra = v; break;
    case 3: c->ddrb = v; break;
    case 4: c->latch_a = (c->latch_a & 0xFF00) | v; break;
    case 5: c->latch_a = (c->latch_a & 0x00FF) | (v << 8);
            { if (!(c->cra & 1)) c->ta = c->latch_a; } break;
    case 6: c->latch_b = (c->latch_b & 0xFF00) | v; break;
    case 7: c->latch_b = (c->latch_b & 0x00FF) | (v << 8);
            { if (!(c->crb & 1)) c->tb = c->latch_b; } break;
    case 8: case 9: case 10: case 11:
      { if (!(c->crb & 0x80)) c->tod[r - 8] = v; } break;
    case 13:
      if (v & 0x80) c->icr_mask |= v & 0x1F; else c->icr_mask &= ~(v & 0x1F);
      upd_irq();
      break;
    case 14:
      if (v & 0x10) c->ta = c->latch_a;
      c->cra = v & ~0x10;
      break;
    case 15:
      if (v & 0x10) c->tb = c->latch_b;
      c->crb = v & ~0x10;
      break;
    default: break;
  }
}

static uint8_t io_rd(uint16_t a) {
  switch ((a >> 8) & 0xF) {
    case 0: case 1: case 2: case 3: return vic_rd(a & 0x3F);
    case 4: case 5: case 6: case 7: {
      int r = a & 0x1F;
      if (r == 0x1B || r == 0x1C) return rnd8();
      if (r >= 0x19 && r <= 0x1A) return 0xFF;
      return 0;
    }
    case 8: case 9: case 10: case 11: return cram[a & 0x3FF] | 0xF0;
    case 12: return cia_rd(&cia1, a & 15, 1);
    case 13: return cia_rd(&cia2, a & 15, 2);
    default: return 0xFF;
  }
}

static void io_wr(uint16_t a, uint8_t v) {
  switch ((a >> 8) & 0xF) {
    case 0: case 1: case 2: case 3: vic_wr(a & 0x3F, v); break;
    case 4: case 5: case 6: case 7: sid[a & 0x1F] = v; break;
    case 8: case 9: case 10: case 11: cram[a & 0x3FF] = v & 0x0F; break;
    case 12: cia_wr(&cia1, a & 15, v); break;
    case 13: cia_wr(&cia2, a & 15, v); break;
    default: break;
  }
}

static inline uint8_t rd(uint16_t a) {
  if (a < 0xA000) {
    if (a > 1) return ram[a];
    return a == 0 ? cpu_ddr : ((cpu_dat & cpu_ddr) | (~cpu_ddr & 0x17));
  }
  if (a < 0xC000) return bk_basic ? basic[a - 0xA000] : ram[a];
  if (a < 0xD000) return ram[a];
  if (a < 0xE000) {
    if (bk_io) return io_rd(a);
    if (bk_char) return chargen[a - 0xD000];
    return ram[a];
  }
  return bk_kernal ? kernal[a - 0xE000] : ram[a];
}

static inline void wr(uint16_t a, uint8_t v) {
  if (a < 2) {
    if (a == 0) cpu_ddr = v; else cpu_dat = v;
    upd_bank();
    return;
  }
  if (a >= 0xD000 && a < 0xE000 && bk_io) { io_wr(a, v); return; }
  ram[a] = v;
}

static inline uint16_t rd16(uint16_t a) { return rd(a) | (rd(a + 1) << 8); }

/* ------------------------------------------------------------------ */
/* CIA timers                                                          */
/* ------------------------------------------------------------------ */
static int tdown(uint16_t *t, uint16_t latch, int n, uint8_t *cr, int oneshot_bit) {
  int u = 0, rem = n;
  while (rem > 0) {
    if (rem <= *t) { *t -= rem; break; }
    rem -= *t + 1;
    *t = latch;
    u++;
    if (*cr & oneshot_bit) { *cr &= ~1; break; }
  }
  return u;
}

static uint8_t bcd_inc(uint8_t v) { return (v & 0xF) == 9 ? (v & 0xF0) + 0x10 : v + 1; }

static void tod_tick(CIA *c) {
  if ((c->tod[0] & 0xF) < 9) { c->tod[0]++; return; }
  c->tod[0] = 0;
  uint8_t s = bcd_inc(c->tod[1]);
  if (s < 0x60) { c->tod[1] = s; return; }
  c->tod[1] = 0;
  uint8_t m = bcd_inc(c->tod[2]);
  if (m < 0x60) { c->tod[2] = m; return; }
  c->tod[2] = 0;
  uint8_t h = c->tod[3], pm = h & 0x80, hh = h & 0x1F;
  hh = bcd_inc(hh);
  if (hh == 0x13) hh = 1;
  if (hh == 0x12) pm ^= 0x80;
  c->tod[3] = pm | hh;
}

static void cia_tick(CIA *c, int n) {
  int ua = 0, ub = 0;
  if ((c->cra & 0x21) == 0x01) ua = tdown(&c->ta, c->latch_a, n, &c->cra, 8);
  if (ua) c->icr_data |= 1;
  if (c->crb & 1) {
    int src = (c->crb >> 5) & 3;
    int cnt = src == 0 ? n : (src >= 2 ? ua : 0);
    if (cnt) ub = tdown(&c->tb, c->latch_b, cnt, &c->crb, 8);
    if (ub) c->icr_data |= 2;
  }
  if (ua || ub) upd_irq();
  c->todcnt += n;
  if (c->todcnt >= 98525) { c->todcnt -= 98525; tod_tick(c); }
}

/* ------------------------------------------------------------------ */
/* 6502                                                                */
/* ------------------------------------------------------------------ */
static inline uint8_t getp(int b) {
  return fC | (fZ << 1) | (fI << 2) | (fD << 3) | (b ? 0x10 : 0) | 0x20 | (fV << 6) | (fN << 7);
}
static inline void setp(uint8_t p) {
  fC = p & 1; fZ = (p >> 1) & 1; fI = (p >> 2) & 1; fD = (p >> 3) & 1;
  fV = (p >> 6) & 1; fN = p >> 7;
}
static inline void push(uint8_t v) { ram[0x100 + sp] = v; sp--; }
static inline uint8_t pop(void) { sp++; return ram[0x100 + sp]; }
static inline void nz(uint8_t v) { fZ = !v; fN = v >> 7; }

static void do_adc(uint8_t v) {
  if (fD) {
    unsigned tmp = (ra & 0xF) + (v & 0xF) + fC;
    if (tmp > 9) tmp += 6;
    if (tmp <= 0x0F) tmp = (tmp & 0xF) + (ra & 0xF0) + (v & 0xF0);
    else tmp = (tmp & 0xF) + (ra & 0xF0) + (v & 0xF0) + 0x10;
    fZ = !((ra + v + fC) & 0xFF);
    fN = (tmp & 0x80) ? 1 : 0;
    fV = (((ra ^ tmp) & 0x80) && !((ra ^ v) & 0x80)) ? 1 : 0;
    if ((tmp & 0x1F0) > 0x90) tmp += 0x60;
    fC = ((tmp & 0xFF0) > 0xF0) ? 1 : 0;
    ra = (uint8_t)tmp;
  } else {
    unsigned r = ra + v + fC;
    fV = ((~(ra ^ v) & (ra ^ r) & 0x80) != 0);
    fC = r > 0xFF;
    ra = (uint8_t)r;
    nz(ra);
  }
}

static void do_sbc(uint8_t v) {
  if (fD) {
    unsigned tmp = ra - v - (fC ? 0 : 1);
    unsigned ta = (ra & 0xF) - (v & 0xF) - (fC ? 0 : 1);
    if (ta & 0x10) ta = ((ta - 6) & 0xF) | ((ra & 0xF0) - (v & 0xF0) - 0x10);
    else ta = (ta & 0xF) | ((ra & 0xF0) - (v & 0xF0));
    if (ta & 0x100) ta -= 0x60;
    fC = tmp < 0x100;
    nz((uint8_t)tmp);
    fV = (((ra ^ tmp) & 0x80) && ((ra ^ v) & 0x80)) ? 1 : 0;
    ra = (uint8_t)ta;
  } else {
    unsigned r = ra + (uint8_t)~v + fC;
    fV = ((ra ^ v) & (ra ^ r) & 0x80) != 0;
    fC = r > 0xFF;
    ra = (uint8_t)r;
    nz(ra);
  }
}

static inline uint8_t do_shift(int kind, uint8_t v) {
  uint8_t c = fC, r;
  switch (kind) {
    case 0: fC = v >> 7; r = v << 1; break;
    case 1: fC = v >> 7; r = (v << 1) | c; break;
    case 2: fC = v & 1; r = v >> 1; break;
    default: fC = v & 1; r = (v >> 1) | (c << 7); break;
  }
  nz(r);
  return r;
}

static inline void do_cmp(uint8_t r, uint8_t v) { uint8_t t = r - v; fC = r >= v; nz(t); }

static void alu(int op, uint8_t v) {
  switch (op) {
    case 0: ra |= v; nz(ra); break;
    case 1: ra &= v; nz(ra); break;
    case 2: ra ^= v; nz(ra); break;
    case 3: do_adc(v); break;
    case 5: ra = v; nz(ra); break;
    case 6: do_cmp(ra, v); break;
    case 7: do_sbc(v); break;
    default: break;
  }
}

/* Addressing modes in "cc=01" numbering:
 * 0 (zp,x)  1 zp  2 imm  3 abs  4 (zp),y  5 zp,x  6 abs,y  7 abs,x */
static inline uint16_t amode(int m, int *cross) {
  uint16_t b, p;
  switch (m) {
    case 0: p = (rd(pc++) + rx) & 0xFF; return rd(p) | (rd((p + 1) & 0xFF) << 8);
    case 1: return rd(pc++);
    case 2: return pc++;
    case 3: b = rd16(pc); pc += 2; return b;
    case 4: p = rd(pc++); b = rd(p) | (rd((p + 1) & 0xFF) << 8);
            *cross = ((b & 0xFF) + ry) > 0xFF; return b + ry;
    case 5: return (rd(pc++) + rx) & 0xFF;
    case 6: b = rd16(pc); pc += 2; *cross = ((b & 0xFF) + ry) > 0xFF; return b + ry;
    default: b = rd16(pc); pc += 2; *cross = ((b & 0xFF) + rx) > 0xFF; return b + rx;
  }
}

static const uint8_t cyc_rd[8]  = {6, 3, 2, 4, 5, 4, 4, 4};
static const uint8_t cyc_wr[8]  = {6, 3, 2, 4, 6, 4, 5, 5};
static const uint8_t cyc_rmw[8] = {8, 5, 2, 6, 8, 6, 7, 7};

static int cpu_step(void) {
  uint8_t op = rd(pc++);
  int cc = op & 3, bbb = (op >> 2) & 7, aaa = op >> 5;
  int cyc = 2, cross = 0;
  uint16_t ea;
  uint8_t v;

  switch (cc) {
  case 1:
    ea = amode(bbb, &cross);
    if (aaa == 4) {
      if (bbb != 2) wr(ea, ra);
      cyc = cyc_wr[bbb];
    } else {
      v = rd(ea);
      alu(aaa, v);
      cyc = cyc_rd[bbb] + (((bbb == 4) || (bbb >= 6)) && cross);
    }
    break;

  case 2:
    if (bbb == 2) {
      switch (aaa) {
        case 0: case 1: case 2: case 3: ra = do_shift(aaa, ra); break;
        case 4: ra = rx; nz(ra); break;
        case 5: rx = ra; nz(rx); break;
        case 6: rx--; nz(rx); break;
        default: break;
      }
      break;
    }
    if (bbb == 0) { if (aaa == 5) { rx = rd(pc++); nz(rx); } else pc++; break; }
    if (bbb == 4) break;
    if (bbb == 6) {
      if (aaa == 4) sp = rx; else if (aaa == 5) { rx = sp; nz(rx); }
      break;
    }
    if (bbb == 5 && (aaa == 4 || aaa == 5)) ea = (rd(pc++) + ry) & 0xFF;
    else if (bbb == 7 && (aaa == 4 || aaa == 5)) ea = amode(6, &cross);
    else ea = amode(bbb, &cross);
    if (aaa == 4) {
      if (bbb != 7) wr(ea, rx);
      cyc = (bbb == 1) ? 3 : (bbb == 7 ? 5 : 4);
    } else if (aaa == 5) {
      rx = rd(ea); nz(rx);
      cyc = (bbb == 1) ? 3 : 4;
      if (bbb == 7 && cross) cyc++;
    } else {
      v = rd(ea);
      if (aaa < 4) v = do_shift(aaa, v);
      else { v = (aaa == 6) ? v - 1 : v + 1; nz(v); }
      wr(ea, v);
      cyc = (bbb == 1) ? 5 : (bbb == 7 ? 7 : 6);
    }
    break;

  case 0:
    switch (bbb) {
    case 0:
      switch (aaa) {
        case 0: pc++; push(pc >> 8); push(pc & 0xFF); push(getp(1)); fI = 1;
                pc = rd16(0xFFFE); cyc = 7; break;
        case 1: { uint16_t t = pc + 1; uint16_t dst = rd16(pc);
                  push(t >> 8); push(t & 0xFF); pc = dst; cyc = 6; } break;
        case 2: setp(pop()); pc = pop(); pc |= pop() << 8; cyc = 6; break;
        case 3: pc = pop(); pc |= pop() << 8; pc++; cyc = 6; break;
        case 4: pc++; break;
        case 5: ry = rd(pc++); nz(ry); break;
        case 6: do_cmp(ry, rd(pc++)); break;
        default: do_cmp(rx, rd(pc++)); break;
      }
      break;
    case 1:
      ea = rd(pc++); cyc = 3;
      switch (aaa) {
        case 1: v = rd(ea); fZ = !(ra & v); fN = v >> 7; fV = (v >> 6) & 1; break;
        case 4: wr(ea, ry); break;
        case 5: ry = rd(ea); nz(ry); break;
        case 6: do_cmp(ry, rd(ea)); break;
        case 7: do_cmp(rx, rd(ea)); break;
        default: break;
      }
      break;
    case 2:
      switch (aaa) {
        case 0: push(getp(1)); cyc = 3; break;
        case 1: setp(pop()); cyc = 4; break;
        case 2: push(ra); cyc = 3; break;
        case 3: ra = pop(); nz(ra); cyc = 4; break;
        case 4: ry--; nz(ry); break;
        case 5: ry = ra; nz(ry); break;
        case 6: ry++; nz(ry); break;
        default: rx++; nz(rx); break;
      }
      break;
    case 3:
      if (aaa == 2) { pc = rd16(pc); cyc = 3; break; }
      if (aaa == 3) {
        uint16_t p = rd16(pc);
        pc = rd(p) | (rd((p & 0xFF00) | ((p + 1) & 0xFF)) << 8);
        cyc = 5; break;
      }
      ea = amode(3, &cross); cyc = 4;
      switch (aaa) {
        case 1: v = rd(ea); fZ = !(ra & v); fN = v >> 7; fV = (v >> 6) & 1; break;
        case 4: wr(ea, ry); break;
        case 5: ry = rd(ea); nz(ry); break;
        case 6: do_cmp(ry, rd(ea)); break;
        case 7: do_cmp(rx, rd(ea)); break;
        default: break;
      }
      break;
    case 4: {
      int flag = (aaa >> 1) == 0 ? fN : (aaa >> 1) == 1 ? fV : (aaa >> 1) == 2 ? fC : fZ;
      int8_t off = (int8_t)rd(pc++);
      if (flag == (aaa & 1)) {
        uint16_t np = pc + off;
        cyc = 3;
        if ((np ^ pc) & 0xFF00) cyc++;
        pc = np;
      }
    } break;
    case 5:
      ea = amode(5, &cross); cyc = 4;
      if (aaa == 4) wr(ea, ry);
      else if (aaa == 5) { ry = rd(ea); nz(ry); }
      break;
    case 6:
      switch (aaa) {
        case 0: fC = 0; break; case 1: fC = 1; break;
        case 2: fI = 0; break; case 3: fI = 1; break;
        case 4: ra = ry; nz(ra); break; case 5: fV = 0; break;
        case 6: fD = 0; break; default: fD = 1; break;
      }
      break;
    default: /* 7 */
      ea = amode(7, &cross); cyc = 4 + cross;
      if (aaa == 5) { ry = rd(ea); nz(ry); }
      else if (aaa == 4) cyc = 5;
      break;
    }
    break;

  default: /* cc == 3: illegal opcodes */
    if (bbb == 2) {
      v = rd(pc++);
      switch (aaa) {
        case 0: case 1: ra &= v; nz(ra); fC = fN; break;           /* ANC */
        case 2: ra &= v; fC = ra & 1; ra >>= 1; nz(ra); break;     /* ALR */
        case 3: ra &= v; ra = (ra >> 1) | (fC << 7); nz(ra);       /* ARR (approx) */
                fC = (ra >> 6) & 1; fV = ((ra >> 6) ^ (ra >> 5)) & 1; break;
        case 4: ra = rx & v; nz(ra); break;                        /* XAA (approx) */
        case 5: ra = rx = v; nz(ra); break;                        /* LAX # */
        case 6: { uint8_t t = ra & rx; fC = t >= v; rx = t - v; nz(rx); } break; /* AXS */
        default: do_sbc(v); break;                                 /* SBC # (EB) */
      }
      break;
    }
    if (bbb == 5 && (aaa == 4 || aaa == 5)) ea = (rd(pc++) + ry) & 0xFF;
    else if (bbb == 7 && (aaa == 4 || aaa == 5)) ea = amode(6, &cross);
    else if (bbb == 7) ea = amode(7, &cross);
    else ea = amode(bbb, &cross);
    if (aaa == 4) {
      if (bbb == 0 || bbb == 1 || bbb == 3 || bbb == 5) wr(ea, ra & rx);   /* SAX */
      cyc = bbb == 0 ? 6 : bbb == 1 ? 3 : 4;
      if (bbb == 4 || bbb == 6 || bbb == 7) cyc = 5;
    } else if (aaa == 5) {
      if (bbb == 6) { cyc = 4; break; }   /* LAS: ignored */
      v = rd(ea); ra = rx = v; nz(ra);
      cyc = cyc_rd[bbb] + (cross && (bbb == 4 || bbb == 7 || bbb == 6));
    } else {
      v = rd(ea);
      switch (aaa) {
        case 0: fC = v >> 7; v <<= 1; wr(ea, v); ra |= v; nz(ra); break;           /* SLO */
        case 1: { uint8_t c = fC; fC = v >> 7; v = (v << 1) | c; wr(ea, v); ra &= v; nz(ra); } break; /* RLA */
        case 2: fC = v & 1; v >>= 1; wr(ea, v); ra ^= v; nz(ra); break;           /* SRE */
        case 3: { uint8_t c = fC; fC = v & 1; v = (v >> 1) | (c << 7); wr(ea, v); do_adc(v); } break; /* RRA */
        case 6: v--; wr(ea, v); do_cmp(ra, v); break;                             /* DCP */
        default: v++; wr(ea, v); do_sbc(v); break;                                /* ISC */
      }
      cyc = cyc_rmw[bbb];
    }
    break;
  }
  return cyc;
}

static int interrupt(uint16_t vec) {
  push(pc >> 8); push(pc & 0xFF); push(getp(0));
  fI = 1;
  pc = rd16(vec);
  return 7;
}

/* ------------------------------------------------------------------ */
/* D64 / LOAD trap                                                     */
/* ------------------------------------------------------------------ */
typedef struct { char name[17]; uint8_t type; uint8_t track, sector; int blocks; } DirEnt;
static DirEnt dir[160];
static int dir_n;

static int spt(int t) { return t < 18 ? 21 : t < 25 ? 19 : t < 31 ? 18 : 17; }
static int sec_off(int t, int s) {
  if (t < 1 || t > 35 || s < 0 || s >= spt(t)) return -1;
  int o = 0;
  for (int i = 1; i < t; i++) o += spt(i);
  return (o + s) * 256;
}

static void d64_scan(void) {
  dir_n = 0;
  if (!d64) return;
  int t = 18, s = 1;
  for (int guard = 0; guard < 20 && t; guard++) {
    int o = sec_off(t, s);
    if (o < 0 || o + 256 > d64len) break;
    const uint8_t *sec = d64 + o;
    for (int e = 0; e < 8 && dir_n < 160; e++) {
      const uint8_t *p = sec + e * 32;
      if (!(p[2] & 7) && !(p[2] & 0x80)) continue;
      if (!(p[2] & 0x80)) continue;                    /* scratched / unclosed */
      DirEnt *d = &dir[dir_n++];
      int n = 0;
      for (int i = 0; i < 16; i++) { if (p[5 + i] == 0xA0) break; d->name[n++] = p[5 + i]; }
      d->name[n] = 0;
      d->type = p[2] & 7;
      d->track = p[3]; d->sector = p[4];
      d->blocks = p[30] | (p[31] << 8);
    }
    t = sec[0]; s = sec[1];
  }
}

static int d64_chain(int t, int s, uint8_t *buf, int max) {
  int n = 0;
  for (int guard = 0; t && guard < 700; guard++) {
    int o = sec_off(t, s);
    if (o < 0 || o + 256 > d64len) break;
    const uint8_t *sec = d64 + o;
    int nt = sec[0], ns = sec[1];
    int cnt = nt ? 254 : (ns >= 1 ? ns - 1 : 0);
    if (n + cnt > max) cnt = max - n;
    memcpy(buf + n, sec + 2, cnt);
    n += cnt;
    if (!nt || cnt < 0) break;
    t = nt; s = ns;
  }
  return n;
}

void c64_mount_d64(const uint8_t *d, int len) {
  d64 = (len >= 174848) ? d : 0;
  d64len = len;
  d64_scan();
}
int c64_d64_count(void) { return dir_n; }
int c64_d64_entry(int i, char *name17, int *blocks) {
  if (i < 0 || i >= dir_n) return 0;
  memcpy(name17, dir[i].name, 17);
  *blocks = dir[i].blocks;
  return 1;
}
int c64_d64_read(int i, uint8_t *buf, int max) {
  if (i < 0 || i >= dir_n) return 0;
  return d64_chain(dir[i].track, dir[i].sector, buf, max);
}

static int glob_match(const char *pat, int plen, const char *name) {
  int i = 0;
  for (; i < plen; i++) {
    if (pat[i] == '*') return 1;
    if (!name[i]) return 0;
    if (pat[i] != '?' && pat[i] != name[i]) return 0;
  }
  return name[i] == 0;
}

static int build_dir(uint8_t *o) {
  int n = 0, start;
  const uint8_t *bam = d64 + sec_off(18, 0);
#define PUT(b) (o[n++] = (uint8_t)(b))
#define LINK() do { uint16_t nx = 0x0801 + n; o[start] = nx & 0xFF; o[start + 1] = nx >> 8; } while (0)
  start = n; PUT(0); PUT(0); PUT(0); PUT(0); PUT(0x12); PUT('"');
  for (int i = 0; i < 16; i++) { uint8_t c = bam[0x90 + i]; PUT(c == 0xA0 ? ' ' : c); }
  PUT('"'); PUT(' ');
  PUT(bam[0xA2]); PUT(bam[0xA3]); PUT(' '); PUT(bam[0xA5]); PUT(bam[0xA6]); PUT(0);
  LINK();
  for (int i = 0; i < dir_n; i++) {
    int b = dir[i].blocks;
    start = n; PUT(0); PUT(0); PUT(b & 0xFF); PUT(b >> 8);
    int pad = b < 10 ? 3 : b < 100 ? 2 : b < 1000 ? 1 : 0;
    while (pad--) PUT(' ');
    PUT('"');
    int l = 0;
    for (; dir[i].name[l]; l++) PUT(dir[i].name[l]);
    PUT('"');
    for (int k = l; k < 16; k++) PUT(' ');
    PUT(' ');
    const char *ty[] = {"DEL", "SEQ", "PRG", "USR", "REL"};
    const char *tn = ty[dir[i].type > 4 ? 0 : dir[i].type];
    PUT(tn[0]); PUT(tn[1]); PUT(tn[2]);
    PUT(' '); PUT(' '); PUT(' ');
    PUT(0);
    LINK();
  }
  int freeb = 0;
  for (int t = 1; t <= 35; t++) if (t != 18) freeb += bam[4 * t];
  start = n; PUT(0); PUT(0); PUT(freeb & 0xFF); PUT(freeb >> 8);
  const char *bf = "BLOCKS FREE.";
  for (const char *p = bf; *p; p++) PUT(*p);
  for (int i = 0; i < 13; i++) PUT(' ');
  PUT(0);
  LINK();
  PUT(0); PUT(0);
#undef PUT
#undef LINK
  return n;
}

static void do_rts(void) { pc = pop(); pc |= pop() << 8; pc++; }

static int load_trap_active(void) { return d64 && ram[0xBA] == 8; }

static void do_load_trap(void) {
  static uint8_t tmp[65536 + 2];
  uint8_t verify = ra;
  int len = ram[0xB7];
  uint16_t fp = ram[0xBB] | (ram[0xBC] << 8);
  char fn[24]; int n = 0;
  for (int i = 0; i < len && n < 20; i++) fn[n++] = ram[(uint16_t)(fp + i)];
  int st = 0;
  if (n >= 2 && fn[1] == ':') st = 2;                      /* "0:" prefix */
  int en = st;
  while (en < n && fn[en] != ',') en++;

  int flen; uint16_t dest, srcoff;
  uint8_t *data;
  if (en - st == 1 && fn[st] == '$') {
    flen = build_dir(tmp + 2);
    tmp[0] = 0x01; tmp[1] = 0x08;
    flen += 2;
    data = tmp;
  } else {
    int found = -1;
    for (int i = 0; i < dir_n; i++)
      if (glob_match(fn + st, en - st, dir[i].name)) { found = i; break; }
    if (found < 0) {
      ra = 4; fC = 1; ram[0x90] = 0x42; do_rts(); return;     /* FILE NOT FOUND */
    }
    flen = d64_chain(dir[found].track, dir[found].sector, tmp, 65536 + 2);
    data = tmp;
  }
  if (flen < 2) { ra = 4; fC = 1; do_rts(); return; }
  dest = ram[0xB9] == 0 ? (ram[0xC3] | (ram[0xC4] << 8)) : (data[0] | (data[1] << 8));
  srcoff = 2;
  uint32_t end = dest;
  if (!verify) {
    for (int i = srcoff; i < flen && end < 0x10000; i++) ram[end++] = data[i];
  } else {
    end = dest + (flen - 2);
  }
  ram[0xAE] = end & 0xFF; ram[0xAF] = (end >> 8) & 0xFF;
  rx = end & 0xFF; ry = (end >> 8) & 0xFF;
  ram[0x90] = 0x40;
  fC = 0;
  do_rts();
}

/* ------------------------------------------------------------------ */
/* Autostart                                                           */
/* ------------------------------------------------------------------ */
static void type_keys(const char *s) {
  int n = 0;
  while (*s && n < 10) ram[0x277 + n++] = (uint8_t)*s++;
  ram[0xC6] = (uint8_t)n;
}

static void do_autostart(void) {
  auto_pending = 0;
  if (auto_len < 3) return;
  uint16_t load = auto_buf[0] | (auto_buf[1] << 8);
  uint32_t end = load;
  for (int i = 2; i < auto_len && end < 0x10000; i++) ram[end++] = auto_buf[i];
  if (load == 0x0801) {
    static const uint8_t zp[] = {0x2D, 0x2F, 0x31, 0xAE};
    for (int i = 0; i < 4; i++) { ram[zp[i]] = end & 0xFF; ram[zp[i] + 1] = (end >> 8) & 0xFF; }
    type_keys("RUN\r");
  } else {
    char s[16] = "SYS"; int n = 3; char d[6]; int k = 0;
    int v = load;
    do { d[k++] = '0' + v % 10; v /= 10; } while (v);
    while (k) s[n++] = d[--k];
    s[n++] = '\r'; s[n] = 0;
    type_keys(s);
  }
}

void c64_autostart_prg(const uint8_t *prg, int len) {
  if (len > (int)sizeof auto_buf) len = sizeof auto_buf;
  memcpy(auto_buf, prg, len);
  auto_len = len;
  c64_reset();
  auto_pending = 1;
}

/* ------------------------------------------------------------------ */
/* VIC-II                                                              */
/* ------------------------------------------------------------------ */
static inline uint8_t vicread(uint16_t a) {
  a &= 0x3FFF;
  if (!(vbank & 0x4000) && a >= 0x1000 && a < 0x2000) return chargen[a - 0x1000];
  return ram[vbank | a];
}

static void fetch_matrix(void) {
  uint16_t vm = (vic[0x18] & 0xF0) << 6;
  for (int i = 0; i < 40; i++) {
    vm_chr[i] = vicread(vm + vcbase + i);
    vm_col[i] = cram[(vcbase + i) & 0x3FF] & 15;
  }
}

static int count_sprites(int line) {
  int c = 0;
  for (int n = 0; n < 8; n++) {
    if (!((vic[0x15] >> n) & 1)) continue;
    int rel = line - vic[1 + 2 * n] - 1;
    if (rel >= 0 && rel < (((vic[0x17] >> n) & 1) ? 42 : 21)) c++;
  }
  return c;
}

static void gen_gfx(uint8_t *p, uint8_t *f) {
  uint8_t d11 = vic[0x11], d16 = vic[0x16], d18 = vic[0x18];
  int ecm = d11 & 0x40, bmm = d11 & 0x20, mcm = d16 & 0x10;
  uint16_t chrb = (d18 & 0x0E) << 10;
  uint16_t bmb = (d18 & 8) ? 0x2000 : 0;
  uint8_t bg0 = vic[0x21] & 15;
  for (int col = 0; col < 40; col++, p += 8, f += 8) {
    uint8_t ch = vm_chr[col], cr = vm_col[col], pat;
    if ((ecm && bmm) || (ecm && mcm)) { memset(p, 0, 8); memset(f, 0, 8); continue; }
    if (!bmm) {
      uint8_t bg = bg0;
      if (ecm) { bg = vic[0x21 + (ch >> 6)] & 15; ch &= 0x3F; }
      pat = vicread(chrb + ch * 8 + rc);
      if (mcm && (cr & 8)) {
        uint8_t cols[4] = {bg0, vic[0x22] & 15, vic[0x23] & 15, cr & 7};
        for (int k = 0; k < 4; k++) {
          int v = (pat >> (6 - 2 * k)) & 3;
          p[2 * k] = p[2 * k + 1] = cols[v];
          f[2 * k] = f[2 * k + 1] = v >> 1;
        }
      } else {
        uint8_t fg = cr & 15;
        for (int i = 0; i < 8; i++) {
          int bit = (pat >> (7 - i)) & 1;
          p[i] = bit ? fg : bg; f[i] = bit;
        }
      }
    } else {
      pat = vicread(bmb + (((vcbase + col) * 8 + rc) & 0x1FFF));
      if (!mcm) {
        uint8_t fg = ch >> 4, bg = ch & 15;
        for (int i = 0; i < 8; i++) {
          int bit = (pat >> (7 - i)) & 1;
          p[i] = bit ? fg : bg; f[i] = bit;
        }
      } else {
        uint8_t cols[4] = {bg0, ch >> 4, ch & 15, cr & 15};
        for (int k = 0; k < 4; k++) {
          int v = (pat >> (6 - 2 * k)) & 3;
          p[2 * k] = p[2 * k + 1] = cols[v];
          f[2 * k] = f[2 * k + 1] = v >> 1;
        }
      }
    }
  }
}

static void draw_sprites(int line, uint8_t *lb, const uint8_t *fm) {
  uint8_t en = vic[0x15];
  if (!en) return;
  uint8_t spc[320], smask[320], spri[320];
  memset(smask, 0, sizeof smask);
  uint16_t vm = (vic[0x18] & 0xF0) << 6;
  int any = 0;
  for (int n = 7; n >= 0; n--) {
    if (!((en >> n) & 1)) continue;
    int ye = (vic[0x17] >> n) & 1;
    int rel = line - vic[1 + 2 * n] - 1;
    if (rel < 0 || rel >= (ye ? 42 : 21)) continue;
    int row = ye ? rel >> 1 : rel;
    uint16_t base = vicread(vm + 0x3F8 + n) * 64 + row * 3;
    uint32_t bits = ((uint32_t)vicread(base) << 16) | (vicread(base + 1) << 8) | vicread(base + 2);
    int xe = (vic[0x1D] >> n) & 1, mc = (vic[0x1C] >> n) & 1, pri = (vic[0x1B] >> n) & 1;
    int sx = (vic[2 * n] | (((vic[0x10] >> n) & 1) << 8)) - 24;
    uint8_t scol = vic[0x27 + n] & 15, m1 = vic[0x25] & 15, m2 = vic[0x26] & 15;
    int w = xe ? 2 : 1;
    any = 1;
    if (!mc) {
      for (int i = 0; i < 24; i++) {
        if (!(bits & (0x800000u >> i))) continue;
        for (int k = 0; k < w; k++) {
          int px = sx + i * w + k;
          if (px < 0 || px >= 320) continue;
          smask[px] |= 1 << n; spc[px] = scol; spri[px] = pri;
        }
      }
    } else {
      for (int i = 0; i < 12; i++) {
        int v = (bits >> (22 - 2 * i)) & 3;
        if (!v) continue;
        uint8_t c = v == 1 ? m1 : v == 2 ? scol : m2;
        for (int k = 0; k < 2 * w; k++) {
          int px = sx + i * 2 * w + k;
          if (px < 0 || px >= 320) continue;
          smask[px] |= 1 << n; spc[px] = c; spri[px] = pri;
        }
      }
    }
  }
  if (!any) return;
  uint8_t ss = 0, sb = 0;
  for (int x = 0; x < 320; x++) {
    uint8_t m = smask[x];
    if (!m) continue;
    if (m & (m - 1)) ss |= m;
    if (fm[x]) sb |= m;
    if (!(spri[x] && fm[x])) lb[x] = spc[x];
  }
  if (ss) { if (!vic[0x1E]) { vic_irq |= 4; upd_irq(); } vic[0x1E] |= ss; }
  if (sb) { if (!vic[0x1F]) { vic_irq |= 2; upd_irq(); } vic[0x1F] |= sb; }
}

static void render_line(int line) {
  uint16_t *dst = &c64_fb[(line - 31) * C64_FB_W];
  uint16_t bcol = pal[vic[0x20] & 15];
  int rsel = (vic[0x11] & 8) != 0;
  int top = rsel ? 51 : 55, bot = rsel ? 250 : 246;
  if (!(vic[0x11] & 0x10) || line < top || line > bot) {
    for (int x = 0; x < 320; x++) dst[x] = bcol;
    return;
  }
  uint8_t g[328], f[328];
  uint8_t bg0 = vic[0x21] & 15;
  int xs = vic[0x16] & 7;
  memset(g, bg0, sizeof g);
  memset(f, 0, sizeof f);
  if (ds) gen_gfx(g + xs, f + xs);
  draw_sprites(line, g, f);
  for (int x = 0; x < 320; x++) dst[x] = pal[g[x]];
  if (!(vic[0x16] & 8)) {
    for (int x = 0; x < 7; x++) dst[x] = bcol;
    for (int x = 311; x < 320; x++) dst[x] = bcol;
  }
}

/* ------------------------------------------------------------------ */
/* Machine                                                             */
/* ------------------------------------------------------------------ */
static int exec(void) {
  int c;
  if (nmi_pending) { nmi_pending = 0; c = interrupt(0xFFFA); }
  else if (irq_line && !fI) c = interrupt(0xFFFE);
  else if (pc == 0xF4A5 && bk_kernal && load_trap_active()) { do_load_trap(); c = 6; }
  else c = cpu_step();
  cia_tick(&cia1, c);
  cia_tick(&cia2, c);
  return c;
}

static void run_line(int line) {
  raster = line;
  vbank = (3 - ((cia2.pra | ~cia2.ddra) & 3)) << 14;
  if (line == 0) { vcbase = 0; den_latch = 0; ds = 0; rc = 0; }
  if (line == 0x30 && (vic[0x11] & 0x10)) den_latch = 1;
  if (line == (vic[0x12] | ((vic[0x11] & 0x80) << 1))) { vic_irq |= 1; upd_irq(); }
  int bad = den_latch && line >= 0x30 && line <= 0xF7 && ((line & 7) == (vic[0x11] & 7));
  if (bad) { ds = 1; rc = 0; fetch_matrix(); }
  int steal = bad ? 43 : 0;
  if (vic[0x15]) steal += 2 * count_sprites(line);
  int budget = 63 - steal;
  if (budget < 8) budget = 8;
  int cyc = carry;
  while (cyc < budget) cyc += exec();
  carry = cyc - budget;
  if (line >= 31 && line <= 270) render_line(line);
  if (ds) { if (rc == 7) { ds = 0; vcbase += 40; } else rc++; }
  if (auto_pending && line == 200 &&
      ((frames >= 80 && pc >= 0xE5CD && pc <= 0xE5D5) || frames >= 600))
    do_autostart();
}

void c64_run_frame(void) {
  for (int l = 0; l < 312; l++) run_line(l);
  frames++;
}

void c64_reset(void) {
  memset(ram, 0, sizeof ram);
  memset(cram, 0, sizeof cram);
  memset(vic, 0, sizeof vic);
  memset(sid, 0, sizeof sid);
  memset(&cia1, 0, sizeof cia1);
  memset(&cia2, 0, sizeof cia2);
  cia1.latch_a = cia1.latch_b = cia1.ta = cia1.tb = 0xFFFF;
  cia2.latch_a = cia2.latch_b = cia2.ta = cia2.tb = 0xFFFF;
  vic_irq = 0; raster = 0; frames = 0; carry = 0;
  vcbase = 0; rc = 0; ds = 0; den_latch = 0;
  cpu_ddr = 0x2F; cpu_dat = 0x37; upd_bank();
  irq_line = 0; nmi_prev = 0; nmi_pending = 0;
  auto_pending = 0;
  ra = rx = ry = 0; sp = 0xFD; fC = fZ = fD = fV = fN = 0; fI = 1;
  pc = rd16(0xFFFC);
}

void c64_init(const uint8_t *k, const uint8_t *b, const uint8_t *c) {
  memcpy(kernal, k, 8192);
  memcpy(basic, b, 8192);
  memcpy(chargen, c, 4096);
  memset(kbd, 0xFF, sizeof kbd);
  joy1 = joy2 = 0xFF;
  c64_reset();
}

const uint8_t *c64_chargen(void) { return chargen; }

void c64_key(int k, int down) {
  int c = k >> 3, r = k & 7;
  if (down) kbd[c] &= ~(1 << r); else kbd[c] |= (1 << r);
}

void c64_joy(int port, int up, int down, int left, int right, int fire) {
  uint8_t v = 0xFF;
  if (up) v &= ~1;
  if (down) v &= ~2;
  if (left) v &= ~4;
  if (right) v &= ~8;
  if (fire) v &= ~16;
  if (port == 1) joy1 = v; else joy2 = v;
}

void c64_restore(void) { nmi_pending = 1; }

void c64_poke(uint16_t a, uint8_t v) { wr(a, v); }
uint8_t c64_peek(uint16_t a) { return rd(a); }

int c64_dbg_exec(uint16_t start, uint16_t stop, int maxsteps) {
  pc = start;
  int n = 0;
  while (pc != stop && n < maxsteps) { cpu_step(); n++; }
  return n;
}
uint8_t c64_dbg_reg(char r) {
  switch (r) {
    case 'a': return ra; case 'x': return rx; case 'y': return ry;
    case 's': return sp; default: return getp(1);
  }
}
