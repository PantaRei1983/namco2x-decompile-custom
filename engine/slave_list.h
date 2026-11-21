/* slave_list.h -- walk the master DSP's display list (see slave_list.c). */
#ifndef ENG_SLAVE_LIST_H
#define ENG_SLAVE_LIST_H
#include <stdint.h>
#include "geo_hw.h"

/* One polygon-RAM word (24-bit value, sign-extended or not), index & 0x7FFF. */
typedef uint32_t (*eng_word_fn)(int index);

typedef struct {
    int head;               /* ENG_LIST_HEAD_SS22 (0x304) or ENG_LIST_HEAD_S22 (0x2FF) */
    int s22_objectflags;    /* System 22: the 0x10 record carries object flags */
    /* optional: the last viewport's projection, for callers that report it */
    int32_t *out_zoom_mant; int *out_zoom_shift; int32_t *out_vx, *out_vy;
    /* optional: a record with code 0xFFF8 is a PDP command block run where it stands (point RAM written on the fly), then the list
     * goes on 0x56 words further (MAME simulate_slavedsp: Alpine Racer 2's spinning ice title); NULL = the record ends the list */
    void (*pdp_in_list)(int offs);
} eng_list_cfg;
#define ENG_LIST_HEAD_SS22 0x304
#define ENG_LIST_HEAD_S22  0x2FF

/* Returns the number of primitives walked. */
int eng_walk_list(eng_word_fn pw, const eng_list_cfg *cfg, geo_quad_cb cb, void *user);
#endif
