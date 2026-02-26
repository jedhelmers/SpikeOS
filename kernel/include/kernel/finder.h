#ifndef _FINDER_H
#define _FINDER_H

/* Open a Finder file manager window at the given directory path.
   Pass NULL or "/" to start at the root. */
void finder_open(const char *path);

#endif
