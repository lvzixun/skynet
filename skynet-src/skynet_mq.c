#include "skynet.h"
#include "skynet_mq.h"
#include "skynet_handle.h"
#include "spinlock.h"
#include "atomic.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdbool.h>

#define MQ_IN_GLOBAL 1
#define MQ_OVERLOAD 1024

struct mq_node {
	struct skynet_message msg;
	ATOM_POINTER next;
};

// Vyukov MPSC lock-free message queue
struct message_queue {
	// Producer side (multi-thread concurrent write)
	ATOM_POINTER tail;

	// Consumer side (single-thread exclusive read)
	struct mq_node *head;
	struct mq_node stub;

	// Queue state
	ATOM_INT length;

	// Preserved fields
	uint32_t handle;
	int release;
	ATOM_INT in_global;
	int overload;
	int overload_threshold;
	struct message_queue *next;
};

struct global_queue {
	struct message_queue *head;
	struct message_queue *tail;
	struct spinlock lock;
};

static struct global_queue *Q = NULL;

void
skynet_globalmq_push(struct message_queue *queue) {
	struct global_queue *q = Q;

	SPIN_LOCK(q)
	assert(queue->next == NULL);
	if (q->tail) {
		q->tail->next = queue;
		q->tail = queue;
	} else {
		q->head = q->tail = queue;
	}
	SPIN_UNLOCK(q)
}

struct message_queue *
skynet_globalmq_pop() {
	struct global_queue *q = Q;

	SPIN_LOCK(q)
	struct message_queue *mq = q->head;
	if (mq) {
		q->head = mq->next;
		if (q->head == NULL) {
			assert(mq == q->tail);
			q->tail = NULL;
		}
		mq->next = NULL;
	}
	SPIN_UNLOCK(q)

	return mq;
}

struct message_queue *
skynet_mq_create(uint32_t handle) {
	struct message_queue *q = skynet_malloc(sizeof(*q));

	ATOM_INIT(&q->stub.next, 0);
	q->head = &q->stub;
	ATOM_INIT(&q->tail, (uintptr_t)&q->stub);

	ATOM_INIT(&q->length, 0);

	q->handle = handle;
	ATOM_INIT(&q->in_global, MQ_IN_GLOBAL);
	q->release = 0;
	q->overload = 0;
	q->overload_threshold = MQ_OVERLOAD;
	q->next = NULL;

	return q;
}

static void
_release(struct message_queue *q) {
	assert(q->next == NULL);
	struct mq_node *node = q->head;
	while (node) {
		struct mq_node *next = (struct mq_node *)ATOM_LOAD(&node->next);
		if (node != &q->stub) {
			skynet_free(node);
		}
		node = next;
	}
	skynet_free(q);
}

uint32_t
skynet_mq_handle(struct message_queue *q) {
	return q->handle;
}

int
skynet_mq_length(struct message_queue *q) {
	return ATOM_LOAD(&q->length);
}

int
skynet_mq_overload(struct message_queue *q) {
	if (q->overload) {
		int overload = q->overload;
		q->overload = 0;
		return overload;
	}
	return 0;
}

int
skynet_mq_pop(struct message_queue *q, struct skynet_message *message) {
	struct mq_node *head = q->head;
	struct mq_node *next = (struct mq_node *)ATOM_LOAD(&head->next);

	if (next == NULL) {
		if (head == (struct mq_node *)ATOM_LOAD(&q->tail)) {
			q->overload_threshold = MQ_OVERLOAD;
			ATOM_STORE(&q->in_global, 0);
			if (head != (struct mq_node *)ATOM_LOAD(&q->tail)) {
				if (ATOM_XCHG(&q->in_global, MQ_IN_GLOBAL) == 0) {
					skynet_globalmq_push(q);
				}
			}
			return 1;
		}
		// Linking window: producer completed exchange but has not written prev->next yet
		do {
			atomic_pause_();
			next = (struct mq_node *)ATOM_LOAD(&head->next);
		} while (next == NULL);
	}

	*message = next->msg;

	q->head = next;

	if (head != &q->stub) {
		skynet_free(head);
	}

	ATOM_FDEC(&q->length);

	int length = ATOM_LOAD(&q->length);
	while (length > q->overload_threshold) {
		q->overload = length;
		q->overload_threshold *= 2;
	}

	return 0;
}

void
skynet_mq_push(struct message_queue *q, struct skynet_message *message) {
	assert(message);

	struct mq_node *node = skynet_malloc(sizeof(*node));
	node->msg = *message;
	ATOM_INIT(&node->next, 0);

	struct mq_node *prev = (struct mq_node *)ATOM_XCHG_POINTER(&q->tail, (uintptr_t)node);

	ATOM_FINC(&q->length);

	ATOM_STORE(&prev->next, (uintptr_t)node);

	if (ATOM_XCHG(&q->in_global, MQ_IN_GLOBAL) == 0) {
		skynet_globalmq_push(q);
	}
}

void
skynet_mq_init() {
	struct global_queue *q = skynet_malloc(sizeof(*q));
	memset(q, 0, sizeof(*q));
	SPIN_INIT(q);
	Q = q;
}

void
skynet_mq_mark_release(struct message_queue *q) {
	assert(q->release == 0);
	q->release = 1;
	if (ATOM_LOAD(&q->in_global) != MQ_IN_GLOBAL) {
		skynet_globalmq_push(q);
	}
}

static void
_drop_queue(struct message_queue *q, message_drop drop_func, void *ud) {
	struct skynet_message msg;
	while (!skynet_mq_pop(q, &msg)) {
		drop_func(&msg, ud);
	}
	_release(q);
}

void
skynet_mq_release(struct message_queue *q, message_drop drop_func, void *ud) {
	if (q->release) {
		_drop_queue(q, drop_func, ud);
	} else {
		skynet_globalmq_push(q);
	}
}
