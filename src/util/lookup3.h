#ifndef SRC_COMMON_UTIL_LOOKUP3_H
#define SRC_COMMON_UTIL_LOOKUP3_H

uint32_t hashword(const uint32_t *k, size_t length, uint32_t initval);
uint32_t hashlittle(const void * key, size_t length, uint32_t initval);
void hashlittle2(const void * key, size_t length, uint32_t * pc, uint32_t * pb);
uint32_t hashbig(const void * key, size_t length, uint32_t initval);

#endif
