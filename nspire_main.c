/* TI-Nspire CX II-T (Ndless) front end for the C64 core.
 * Build with the Ndless SDK (see Makefile / README.md).
 * NOTE: written against libndls from memory and NOT compiled against the SDK;
 * if a KEY_NSPIRE_* name is missing in your keys.h, change or delete that line. */
#include <os.h>
#include <libndls.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "c64.h"

#define DIR_PATH "/documents/c64/"
#define RTC_SEC (*(volatile unsigned *)0x90090000)

/* ---------------- files ---------------- */
static uint8_t *load_file(const char *path, int *len) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t *b = malloc(n > 0 ? n : 1);
  if (b && fread(b, 1, n, f) != (size_t)n) { free(b); b = NULL; }
  fclose(f);
  if (len) *len = (int)n;
  return b;
}

static uint8_t *rom(const char *name, int size) {
  char p[96];
  snprintf(p, sizeof p, DIR_PATH "%s.tns", name);
  int n = 0;
  uint8_t *b = load_file(p, &n);
  if (b && n != size) { free(b); b = NULL; }
  return b;
}

/* ---------------- text drawing using the C64 character ROM ---------------- */
static void draw_text(int x, int y, const char *s, uint16_t fg, uint16_t bg) {
  const uint8_t *cg = c64_chargen();
  for (; *s && x + 8 <= C64_FB_W; s++, x += 8) {
    int c = (unsigned char)*s, sc;
    if (c >= 'a' && c <= 'z') c -= 32;
    sc = (c >= 'A' && c <= 'Z') ? c - 64 : (c >= 32 && c < 64) ? c : 32;
    if (c == '@') sc = 0;
    for (int r = 0; r < 8; r++) {
      uint8_t p = cg[sc * 8 + r];
      for (int i = 0; i < 8; i++)
        c64_fb[(y + r) * C64_FB_W + x + i] = (p & (0x80 >> i)) ? fg : bg;
    }
  }
}

static void present(void) { lcd_blit(c64_fb, SCR_320x240_565); }

static void wait_release(void) { while (any_key_pressed()) msleep(10); }

/* ---------------- menu ---------------- */
typedef struct { char label[40]; } Item;

/* returns selected index or -1 for cancel */
static int menu(const char *title, Item *items, int n) {
  int sel = 0, top = 0;
  const int rows = 24;
  for (;;) {
    for (int i = 0; i < C64_FB_W * C64_FB_H; i++) c64_fb[i] = 0x0010;
    draw_text(8, 4, title, 0xFFFF, 0x0010);
    draw_text(8, 224, "UP/DOWN  ENTER=SELECT  ESC=BACK", 0xBDF7, 0x0010);
    if (sel < top) top = sel;
    if (sel >= top + rows) top = sel - rows + 1;
    for (int i = 0; i < rows && top + i < n; i++) {
      int hl = (top + i == sel);
      draw_text(8, 20 + i * 8, items[top + i].label, hl ? 0x0010 : 0xFFFF, hl ? 0xFFE0 : 0x0010);
    }
    present();
    wait_release();
    while (!any_key_pressed()) msleep(10);
    if (isKeyPressed(KEY_NSPIRE_UP) && sel > 0) sel--;
    else if (isKeyPressed(KEY_NSPIRE_DOWN) && sel < n - 1) sel++;
    else if (isKeyPressed(KEY_NSPIRE_ENTER) || isKeyPressed(KEY_NSPIRE_CLICK)) return sel;
    else if (isKeyPressed(KEY_NSPIRE_ESC)) return -1;
    msleep(120);
  }
}

static int ends_with(const char *s, const char *suf) {
  int a = strlen(s), b = strlen(suf);
  if (a < b) return 0;
  for (int i = 0; i < b; i++) {
    char x = s[a - b + i], y = suf[i];
    if (x >= 'A' && x <= 'Z') x += 32;
    if (x != y) return 0;
  }
  return 1;
}

/* ---------------- input ---------------- */
static int cursor_mode, port_sel = 2;

#define KM(nk, ck) c64_key((ck), isKeyPressed(nk))

static void scan_keys(void) {
  int ctrl = isKeyPressed(KEY_NSPIRE_CTRL);
  int shift = isKeyPressed(KEY_NSPIRE_SHIFT);
  int up = isKeyPressed(KEY_NSPIRE_UP), down = isKeyPressed(KEY_NSPIRE_DOWN);
  int left = isKeyPressed(KEY_NSPIRE_LEFT), right = isKeyPressed(KEY_NSPIRE_RIGHT);
  int fire = isKeyPressed(KEY_NSPIRE_CLICK);

  KM(KEY_NSPIRE_A, K_A); KM(KEY_NSPIRE_B, K_B); KM(KEY_NSPIRE_C, K_C); KM(KEY_NSPIRE_D, K_D);
  KM(KEY_NSPIRE_E, K_E); KM(KEY_NSPIRE_F, K_F); KM(KEY_NSPIRE_G, K_G); KM(KEY_NSPIRE_H, K_H);
  KM(KEY_NSPIRE_I, K_I); KM(KEY_NSPIRE_J, K_J); KM(KEY_NSPIRE_K, K_K); KM(KEY_NSPIRE_L, K_L);
  KM(KEY_NSPIRE_M, K_M); KM(KEY_NSPIRE_N, K_N); KM(KEY_NSPIRE_O, K_O); KM(KEY_NSPIRE_P, K_P);
  KM(KEY_NSPIRE_Q, K_Q); KM(KEY_NSPIRE_R, K_R); KM(KEY_NSPIRE_S, K_S); KM(KEY_NSPIRE_T, K_T);
  KM(KEY_NSPIRE_U, K_U); KM(KEY_NSPIRE_V, K_V); KM(KEY_NSPIRE_W, K_W); KM(KEY_NSPIRE_X, K_X);
  KM(KEY_NSPIRE_Y, K_Y); KM(KEY_NSPIRE_Z, K_Z);

  /* Ctrl+1..4 = F1/F3/F5/F7, otherwise digits */
  c64_key(K_F1, ctrl && isKeyPressed(KEY_NSPIRE_1));
  c64_key(K_F3, ctrl && isKeyPressed(KEY_NSPIRE_2));
  c64_key(K_F5, ctrl && isKeyPressed(KEY_NSPIRE_3));
  c64_key(K_F7, ctrl && isKeyPressed(KEY_NSPIRE_4));
  c64_key(K_1, !ctrl && isKeyPressed(KEY_NSPIRE_1));
  c64_key(K_2, !ctrl && isKeyPressed(KEY_NSPIRE_2));
  c64_key(K_3, !ctrl && isKeyPressed(KEY_NSPIRE_3));
  c64_key(K_4, !ctrl && isKeyPressed(KEY_NSPIRE_4));
  KM(KEY_NSPIRE_5, K_5); KM(KEY_NSPIRE_6, K_6); KM(KEY_NSPIRE_7, K_7);
  KM(KEY_NSPIRE_8, K_8); KM(KEY_NSPIRE_9, K_9); KM(KEY_NSPIRE_0, K_0);

  KM(KEY_NSPIRE_SPACE, K_SPACE);
  KM(KEY_NSPIRE_ENTER, K_RET);
  KM(KEY_NSPIRE_DEL, K_DEL);
  KM(KEY_NSPIRE_COMMA, K_COMMA);
  KM(KEY_NSPIRE_PERIOD, K_DOT);
  KM(KEY_NSPIRE_PLUS, K_PLUS);
  KM(KEY_NSPIRE_MINUS, K_MINUS);
  KM(KEY_NSPIRE_MULTIPLY, K_STAR);
  KM(KEY_NSPIRE_DIVIDE, K_SLASH);
  KM(KEY_NSPIRE_EQU, K_EQ);
  KM(KEY_NSPIRE_TAB, K_CBM);
  c64_key(K_STOP, !ctrl && isKeyPressed(KEY_NSPIRE_ESC));

  if (cursor_mode) {
    c64_key(K_CDOWN, down || up);
    c64_key(K_CRIGHT, right || left);
    c64_key(K_LSHIFT, shift || up || left);
    c64_joy(1, 0, 0, 0, 0, 0);
    c64_joy(2, 0, 0, 0, 0, fire);
  } else {
    c64_key(K_CDOWN, 0); c64_key(K_CRIGHT, 0);
    c64_key(K_LSHIFT, shift);
    if (port_sel == 1) { c64_joy(1, up, down, left, right, fire); c64_joy(2, 0, 0, 0, 0, 0); }
    else               { c64_joy(2, up, down, left, right, fire); c64_joy(1, 0, 0, 0, 0, 0); }
  }
}

/* ---------------- frame pacing (RTC based, ~50 fps) ---------------- */
static volatile unsigned spin_sink;
static void spin(unsigned n) { for (unsigned i = 0; i < n; i++) spin_sink += i; }

static unsigned calibrate(void) {
  unsigned t = RTC_SEC;
  while (RTC_SEC == t) ;            /* align to a second boundary */
  t = RTC_SEC;
  unsigned long loops = 0;
  while (RTC_SEC == t) { spin(20000); loops++; }
  return (unsigned)(loops * 20000UL);   /* spin iterations per second */
}

static unsigned ips, delay_iters, last_sec, frames_this_sec;

static void pace(void) {
  spin(delay_iters);
  frames_this_sec++;
  unsigned s = RTC_SEC;
  if (s != last_sec) {
    if (frames_this_sec > 0 && ips) {
      int d = (int)(ips / 50) - (int)(ips / frames_this_sec);
      long nd = (long)delay_iters + d;
      delay_iters = nd < 0 ? 0 : (unsigned)nd;
    }
    last_sec = s;
    frames_this_sec = 0;
  }
}

/* ---------------- main ---------------- */
static void run_emulation(void) {
  wait_release();
  while (!(isKeyPressed(KEY_NSPIRE_CTRL) && isKeyPressed(KEY_NSPIRE_ESC))) {
    static int menu_prev;
    int m = isKeyPressed(KEY_NSPIRE_MENU);
    if (m && !menu_prev) {
      if (isKeyPressed(KEY_NSPIRE_CTRL)) port_sel = 3 - port_sel;
      else cursor_mode = !cursor_mode;
    }
    menu_prev = m;
    scan_keys();
    c64_run_frame();
    present();
    pace();
  }
  wait_release();
}

int main(void) {
  uint8_t *k = rom("kernal.bin", 8192), *b = rom("basic.bin", 8192), *c = rom("chargen.bin", 4096);
  if (!k || !b || !c) {
    show_msgbox("C64", "Missing ROMs. Put kernal.bin.tns (8192 bytes), basic.bin.tns (8192) and chargen.bin.tns (4096) in /documents/c64/");
    return 1;
  }
  if (!lcd_init(SCR_320x240_565)) { show_msgbox("C64", "Could not set 320x240 16-bit LCD mode"); return 1; }
  c64_init(k, b, c);
  ips = calibrate();
  last_sec = RTC_SEC;

  static Item files[200];
  static char names[200][48];
  int nf = 0;
  strcpy(files[nf].label, "[ BOOT TO BASIC ]");
  strcpy(names[nf++], "");
  DIR *d = opendir(DIR_PATH);
  struct dirent *e;
  while (d && (e = readdir(d)) && nf < 200) {
    if (ends_with(e->d_name, ".prg.tns") || ends_with(e->d_name, ".d64.tns")) {
      strncpy(names[nf], e->d_name, 47); names[nf][47] = 0;
      strncpy(files[nf].label, e->d_name, 39); files[nf].label[39] = 0;
      files[nf].label[strlen(e->d_name) - 4] = 0;       /* hide .tns */
      nf++;
    }
  }
  if (d) closedir(d);

  for (;;) {
    int sel = menu("C64 - SELECT PROGRAM", files, nf);
    if (sel < 0) break;
    if (sel == 0) { c64_reset(); run_emulation(); continue; }
    char path[128];
    snprintf(path, sizeof path, DIR_PATH "%s", names[sel]);
    int len = 0;
    uint8_t *data = load_file(path, &len);
    if (!data) { show_msgbox("C64", "Could not read file"); continue; }
    if (ends_with(names[sel], ".prg.tns")) {
      c64_autostart_prg(data, len);
      free(data);
      run_emulation();
    } else {
      static uint8_t *disk;                 /* must stay valid while mounted */
      if (disk) free(disk);
      disk = data;
      c64_mount_d64(disk, len);
      int cnt = c64_d64_count();
      if (cnt == 0) { show_msgbox("C64", "No files found on disk image"); continue; }
      static Item de[160];
      for (int i = 0; i < cnt && i < 160; i++) {
        char nm[17]; int bl;
        c64_d64_entry(i, nm, &bl);
        snprintf(de[i].label, sizeof de[i].label, "%-16s %4d", nm, bl);
      }
      int s2 = menu("DISK - SELECT FILE", de, cnt < 160 ? cnt : 160);
      if (s2 < 0) continue;
      static uint8_t buf[65536 + 2];
      int n = c64_d64_read(s2, buf, sizeof buf);
      c64_autostart_prg(buf, n);
      run_emulation();
    }
  }
  lcd_init(SCR_TYPE_INVALID);
  return 0;
}
