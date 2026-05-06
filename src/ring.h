#ifndef AC_RING_H
#define AC_RING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct Ring Ring;

Ring *ring_create(size_t capacity_elements, size_t element_size);
void  ring_destroy(Ring *r);

size_t   ring_capacity(const Ring *r);
size_t   ring_size(const Ring *r);
bool     ring_empty(const Ring *r);
bool     ring_full(const Ring *r);

uint64_t ring_push(Ring *r, const void *element);
bool     ring_get(const Ring *r, size_t offset, void *out);

uint64_t ring_oldest_serial(const Ring *r);
uint64_t ring_newest_serial(const Ring *r);

bool ring_get_by_serial(const Ring *r, uint64_t serial, void *out);

#endif
