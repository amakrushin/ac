#include "ring.h"

#include <stdlib.h>
#include <string.h>

struct Ring {
    size_t   capacity;
    size_t   element_size;
    size_t   size;
    size_t   head;
    uint64_t next_serial;
    unsigned char *data;
};

Ring *ring_create(size_t capacity_elements, size_t element_size) {
    if (capacity_elements == 0 || element_size == 0) return NULL;
    Ring *r = calloc(1, sizeof(*r));
    if (!r) return NULL;
    r->data = calloc(capacity_elements, element_size);
    if (!r->data) { free(r); return NULL; }
    r->capacity     = capacity_elements;
    r->element_size = element_size;
    r->next_serial  = 1;
    return r;
}

void ring_destroy(Ring *r) {
    if (!r) return;
    free(r->data);
    free(r);
}

size_t ring_capacity(const Ring *r) { return r ? r->capacity     : 0; }
size_t ring_size(const Ring *r)     { return r ? r->size         : 0; }
bool   ring_empty(const Ring *r)    { return r ? r->size == 0    : true; }
bool   ring_full(const Ring *r)     { return r ? r->size == r->capacity : false; }

uint64_t ring_push(Ring *r, const void *element) {
    if (!r || !element) return 0;
    size_t slot;
    if (r->size < r->capacity) {
        slot = (r->head + r->size) % r->capacity;
        r->size++;
    } else {
        slot = r->head;
        r->head = (r->head + 1) % r->capacity;
    }
    memcpy(r->data + slot * r->element_size, element, r->element_size);
    return r->next_serial++;
}

bool ring_get(const Ring *r, size_t offset, void *out) {
    if (!r || !out || offset >= r->size) return false;
    size_t slot = (r->head + offset) % r->capacity;
    memcpy(out, r->data + slot * r->element_size, r->element_size);
    return true;
}

uint64_t ring_oldest_serial(const Ring *r) {
    if (!r || r->size == 0) return 0;
    return r->next_serial - r->size;
}

uint64_t ring_newest_serial(const Ring *r) {
    if (!r || r->size == 0) return 0;
    return r->next_serial - 1;
}

bool ring_get_by_serial(const Ring *r, uint64_t serial, void *out) {
    if (!r || !out || r->size == 0) return false;
    uint64_t oldest = ring_oldest_serial(r);
    uint64_t newest = ring_newest_serial(r);
    if (serial < oldest || serial > newest) return false;
    return ring_get(r, (size_t)(serial - oldest), out);
}
