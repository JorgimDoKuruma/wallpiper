/*
 * SPDX-License-Identifier: MIT
 *
 * Undo the LC_ALL=C that Proton's steam.exe hands to the game process.
 *
 * Steam launches games with LC_ALL=C and keeps the user's real value in
 * HOST_LC_ALL; the proton script restores it for itself, but steam.exe
 * re-applies LC_ALL=C (and LC_MESSAGES=en_US.UTF-8) when it starts
 * wallpaper64.exe. Wine derives the Unix codeset it uses for file names from
 * the C library locale, so under LC_ALL=C any wallpaper whose file name has
 * non-ASCII characters (kanji, hanzi, accents...) is reported as missing
 * (Media Foundation returns 0x80070002) and never plays.
 *
 * This runs as a constructor of the LD_PRELOAD'ed library, i.e. before Wine's
 * own initialisation calls setlocale(), so Wine sees the user's locale.
 */

#include <stdlib.h>
#include <string.h>

__attribute__((constructor)) static void wp_fix_locale(void) {
  const char *all = getenv("LC_ALL");
  if (!all || strcmp(all, "C") != 0) {
    return;
  }

  const char *host = getenv("HOST_LC_ALL");
  if (host && host[0] != '\0') {
    setenv("LC_ALL", host, 1);
  } else {
    unsetenv("LC_ALL");
  }
}
