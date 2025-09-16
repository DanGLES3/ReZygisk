#ifndef MISC_H
#define MISC_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#include <inttypes.h>

#define IS_ISOLATED_SERVICE(uid)      \
  ((uid) >= 90000 && (uid) < 1000000)

struct kernel_version {
  uint8_t major;
  unsigned int minor;
  unsigned int patch;
};

/*
 * Bionic's atoi runs through strtol().
 * Use our own implementation for faster conversion.
 */
int parse_int(const char *str);

struct kernel_version parse_kversion();

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* MISC_H */