#ifndef _TETRIS_H
#define _TETRIS_H

/* Run the Tetris game in the current thread.
   Switches to VGA Mode 13h on entry and restores text mode on exit.
   Controls: A/D = left/right, W = rotate, S = soft-drop,
             Space = hard-drop, Q = quit. */
void tetris_run(void);

#endif
