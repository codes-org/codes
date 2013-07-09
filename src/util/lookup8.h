#ifndef SRC_COMMON_UTIL_LOOKUP8_H
#define SRC_COMMON_UTIL_LOOKUP8_H

typedef  unsigned long  long ub8;   /* unsigned 8-byte quantities */
typedef  unsigned long  int  ub4;   /* unsigned 4-byte quantities */
typedef  unsigned       char ub1;

ub8 hash(register const ub1 * k, register ub8 length, register ub8 level);
ub8 hash2(register ub8 * k2, register ub8 length2, register ub8 level2);
ub8 hash3(register ub1 * k3, register ub8 length3, register ub8 level3);

#endif
