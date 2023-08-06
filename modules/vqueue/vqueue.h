/**
 * @file vqueue.h Audio module that implements a priority queue with loops
 * and interuption modes
 *
 * Copyright (C) 2023 Lars Immisch
 */

#ifndef VQUEUE_INCLUDED_H
#define VQUEUE_INCLUDED_H

extern "C" {
	// returns an ID that can be used to stop the molecule
	size_t vqueue_enqueue(const char* args);
	void vqueue_stop(const char* args);
	void vqueue_cancel(const char* args);
};

#endif // VQUEUE_INCLUDED_H
