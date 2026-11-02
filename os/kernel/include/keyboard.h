#ifndef KEYBOARD_H
#define KEYBOARD_H

/* v1.1.1: logical key codes. The input layer parses arrow/Home/End escapes
 * ONCE, here, and delivers single bytes -- PS/2 scancodes and the terminal's
 * ANSI sequences (ESC [ A/B/C/D/H/F, ESC O H/F) both collapse into these.
 * Both line editors (kernel shell, ulib's ureadline_ex) read one grammar;
 * the codes sit above printable ASCII so they can never collide with text. */
#define KEY_UP     0x80
#define KEY_DOWN   0x81
#define KEY_RIGHT  0x82
#define KEY_LEFT   0x83
#define KEY_HOME   0x84
#define KEY_END    0x85

void keyboard_init(void);
void keyboard_irq(void);          /* called from IRQ1 (PS/2) */
void keyboard_serial_irq(void);   /* called from IRQ4 (COM1 rx) */
void keyboard_serial_drain(void); /* poll COM1 rx into the input queue */
int keyboard_getchar(void);       /* blocking; returns ascii or -1 */
int keyboard_trychar(void);       /* non-blocking; ascii or -1 if empty */

/* v1.2.0: the console's foreground process group (Ctrl-C routing). osh
 * registers the running job's group via ioctl TCSETPGRP; Ctrl-C at the
 * input layer becomes SIGINT for that group and never reaches a reader. */
void keyboard_set_fg_pgid(int pgid);
int keyboard_fg_pgid(void);

#endif /* KEYBOARD_H */
