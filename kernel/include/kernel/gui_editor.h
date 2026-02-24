#ifndef _GUI_EDITOR_H
#define _GUI_EDITOR_H

/* Open a GUI text editor in a new window for the given file path.
   Spawns a kernel thread; returns immediately. */
void gui_editor_open(const char *filename);

#endif
