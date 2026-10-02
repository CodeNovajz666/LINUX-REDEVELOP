#ifndef KEYBOARD_H
#define KEYBOARD_H

void keyboard_init(void);
void keyboard_irq(void);          /* called from IRQ1 (PS/2) */
void keyboard_serial_irq(void);   /* called from IRQ4 (COM1 rx) */
void keyboard_serial_drain(void); /* poll COM1 rx into the input queue */
int keyboard_getchar(void);       /* blocking; returns ascii or -1 */
int keyboard_trychar(void);       /* non-blocking; ascii or -1 if empty */

#endif /* KEYBOARD_H */
