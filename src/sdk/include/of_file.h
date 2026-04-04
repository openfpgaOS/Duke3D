/*
 * of_file.h -- File I/O for openfpgaOS
 *
 * Use standard C fopen/fread/fwrite/fclose/fseek/ftell.
 *   fopen("slot:3", "rb")     -- opens data slot 3
 *   fopen("save:0", "wb")     -- opens save slot 0 for writing
 *   fclose(f)                  -- auto-flushes saves
 *
 * Use opendir/readdir to discover available files.
 */

#ifndef OF_FILE_H
#define OF_FILE_H

/* No of_file_* functions — use POSIX file I/O. */

#endif /* OF_FILE_H */
