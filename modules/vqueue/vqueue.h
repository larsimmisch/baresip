/**
 * @file vqueue.h Audio module that implements a priority queue with loops
 * and interuption modes
 *
 * Copyright (C) 2023 Lars Immisch
 */

#ifndef VQUEUE_INCLUDED_H
#define VQUEUE_INCLUDED_H

#ifdef __cplusplus
extern "C" {
#endif

int vqueue_play_alloc(struct auplay_st **stp,
	const struct auplay *ap, struct auplay_prm *prm,
	const char *dev, auplay_write_h *wh, void *arg);

int vqueue_src_alloc(struct ausrc_st **stp, const struct ausrc *as,
			struct ausrc_prm *prm, const char *dev,
			ausrc_read_h *rh, ausrc_error_h *errh, void *arg);

// returns an ID that can be used to stop the molecule
int vqueue_enqueue(const char* args);
void vqueue_stop(const char* args);
void vqueue_cancel(const char* args);

#ifdef __cplusplus
};
#endif

#endif // VQUEUE_INCLUDED_H
