/*
 * of_save.h -- Save file API for openfpgaOS
 *
 * Use standard C fopen/fread/fwrite/fclose with "save:N" paths.
 * fclose() auto-flushes written data to SD card.
 *
 * Example:
 *   FILE *f = fopen("save:0", "wb");
 *   fwrite(data, 1, size, f);
 *   fclose(f);  // auto-flush to SD
 *
 *   f = fopen("save:0", "rb");
 *   fread(data, 1, size, f);
 *   fclose(f);
 */

#ifndef OF_SAVE_H
#define OF_SAVE_H

/* No of_save_* functions — use POSIX file I/O with "save:N" paths. */

#endif /* OF_SAVE_H */
