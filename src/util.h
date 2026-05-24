/*
 * util.h -- misc utility functions
 */

#ifndef __UTIL_H__
#define __UTIL_H__

#include <stdint.h>

int debugPrintf(const char *text, ...);

int ret0(void);
int ret1(void);
int retm1(void);

#endif
