#ifndef _EDITOR_H
#define _EDITOR_H

/* Open a nano-like text editor for the given filename.
   Blocks until the user exits (Ctrl+X). */
void editor_run(const char *filename);

#endif
