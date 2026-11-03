#ifndef TERMINAL_H
#define TERMINAL_H

// Takes over the full screen with the classic HasOS text shell (every
// existing command works exactly as before) until Esc is pressed, then
// returns. Clobbers the framebuffer, so the caller (the taskbar's Start
// menu) is responsible for redrawing the desktop afterward.
void terminal_run(void);

#endif
