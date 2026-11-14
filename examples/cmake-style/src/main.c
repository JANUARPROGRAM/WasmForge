#include <stdio.h>
#include "config.h"
#ifdef HAVE_LIBPNG
#include <png.h>
#endif
#ifdef HAVE_LIBSAMPLERATE
#include <samplerate.h>
#endif

int main(void) {
  printf("%s %s\n", PACKAGE_NAME, PACKAGE_VERSION);
  return ENABLE_SOUND;
}
