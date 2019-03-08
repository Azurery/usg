#ifndef _USG_RING_H_
#define _USG_RING_H_

#ifdef __cplusplus
extern "C" {
#endif

enum usg_ring_queue_behavior {
	USG_RING_QUEUE_FIXED = 0, /* Enq/Deq a fixed number of items from a ring */
	USG_RING_QUEUE_VARIABLE   /* Enq/Deq as many items as possible from ring */
};

struct usg_ring_headtail {
	volatile uint32_t head;  /**< Prod/consumer head. */
	volatile uint32_t tail;  /**< Prod/consumer tail. */
	uint32_t single;         /**< True if single prod/cons */
};

struct usg_ring {
	int flags;               /**< Flags supplied at creation. */
	uint32_t size;           /**< Size of ring. */
	uint32_t mask;           /**< Mask (size-1) of ring. */
	uint32_t capacity;       /**< Usable size of ring */

	char pad0 __usg_cache_aligned; /**< empty cache line */

	/** Ring producer status. */
	struct usg_ring_headtail prod __usg_cache_aligned;
	char pad1 __usg_cache_aligned; /**< empty cache line */

	/** Ring consumer status. */
	struct usg_ring_headtail cons __usg_cache_aligned;
	char pad2 __usg_cache_aligned; /**< empty cache line */
};

#define RING_F_SP_ENQ 0x0001 /**< The default enqueue is "single-producer". */
#define RING_F_SC_DEQ 0x0002 /**< The default dequeue is "single-consumer". */
#define RING_F_EXACT_SZ 0x0004
#define USG_RING_SZ_MASK  (0x7fffffffU) /**< Ring size mask */

#define __IS_SP 1
#define __IS_MP 0
#define __IS_SC 1
#define __IS_MC 0

ssize_t usg_ring_get_memsize(unsigned count);

int usg_ring_init(struct usg_ring *r, unsigned count, unsigned flags);
struct usg_ring *usg_ring_create(unsigned count, unsigned flags);
void usg_ring_free(struct usg_ring *r);
void usg_ring_dump(FILE *f, const struct usg_ring *r);

#define ENQUEUE_PTRS(r, ring_start, prod_head, obj_table, n, obj_type) do { \
	unsigned int i; \
	const uint32_t size = (r)->size; \
	uint32_t idx = prod_head & (r)->mask; \
	obj_type *ring = (obj_type *)ring_start; \
	if (likely(idx + n < size)) { \
		for (i = 0; i < (n & ((~(unsigned)0x3))); i+=4, idx+=4) { \
			ring[idx] = obj_table[i]; \
			ring[idx+1] = obj_table[i+1]; \
			ring[idx+2] = obj_table[i+2]; \
			ring[idx+3] = obj_table[i+3]; \
		} \
		switch (n & 0x3) { \
		case 3: \
			ring[idx++] = obj_table[i++]; /* fallthrough */ \
		case 2: \
			ring[idx++] = obj_table[i++]; /* fallthrough */ \
		case 1: \
			ring[idx++] = obj_table[i++]; \
		} \
	} else { \
		for (i = 0; idx < size; i++, idx++)\
			ring[idx] = obj_table[i]; \
		for (idx = 0; i < n; i++, idx++) \
			ring[idx] = obj_table[i]; \
	} \
} while (0)

#define DEQUEUE_PTRS(r, ring_start, cons_head, obj_table, n, obj_type) do { \
	unsigned int i; \
	uint32_t idx = cons_head & (r)->mask; \
	const uint32_t size = (r)->size; \
	obj_type *ring = (obj_type *)ring_start; \
	if (likely(idx + n < size)) { \
		for (i = 0; i < (n & (~(unsigned)0x3)); i+=4, idx+=4) {\
			obj_table[i] = ring[idx]; \
			obj_table[i+1] = ring[idx+1]; \
			obj_table[i+2] = ring[idx+2]; \
			obj_table[i+3] = ring[idx+3]; \
		} \
		switch (n & 0x3) { \
		case 3: \
			obj_table[i++] = ring[idx++]; /* fallthrough */ \
		case 2: \
			obj_table[i++] = ring[idx++]; /* fallthrough */ \
		case 1: \
			obj_table[i++] = ring[idx++]; \
		} \
	} else { \
		for (i = 0; idx < size; i++, idx++) \
			obj_table[i] = ring[idx]; \
		for (idx = 0; i < n; i++, idx++) \
			obj_table[i] = ring[idx]; \
	} \
} while (0)

static __al_inline void
update_tail(struct usg_ring_headtail *ht, uint32_t old_val, uint32_t new_val,
		uint32_t single, uint32_t enqueue)
{
	if (enqueue)
		usg_wmb();
	else
		usg_rmb();

    /* ����Ƕ����������̣�������Ҫ�ȴ�tail����������Ҫ��old val
       ��Ϊ�Ƕ������ߣ�������Ҫ������prod�����tail update�������
       �ܳ���*/
	if (!single)
		while (unlikely(ht->tail != old_val))
			_mm_pause();

    /* ����tail */
	ht->tail = new_val;
}

static __al_inline unsigned int
__usg_ring_move_prod_head(struct usg_ring *r, unsigned int is_sp,
		unsigned int n, enum usg_ring_queue_behavior behavior,
		uint32_t *old_head, uint32_t *new_head,
		uint32_t *free_entries)
{
	const uint32_t capacity = r->capacity;
	unsigned int max = n;
	int success;

	do {
		n = max;

		*old_head = r->prod.head;

		usg_rmb();

		/* ���㵱ǰ����������
		   cons.tail��С�ڵ���prod.head, ����r->cons.tail - *old_head�õ�һ��
		   ������capacity�������ֵ�͵õ�ʣ������� */
		*free_entries = (capacity + r->cons.tail - *old_head);

		if (unlikely(n > *free_entries))
			n = (behavior == USG_RING_QUEUE_FIXED) ?
					0 : *free_entries;

		if (n == 0)
			return 0;

        /* ��ͷ��λ�� */
		*new_head = *old_head + n;
        /* ����ǵ������ߣ�ֱ�Ӹ���r->prod.head���ɣ�����Ҫ���� */
		if (is_sp)
			r->prod.head = *new_head, success = 1;
        /* ����Ƕ������ߣ���Ҫʹ��cmpset�Ƚϣ����&r->prod.head == *old_head
           ��&r->prod.head = *new_head
           ��������ѭ������ȡ�µ�*old_head = r->prod.head��֪���ɹ�λ��*/
		else
			success = usg_atomic32_cmpset(&r->prod.head,
					*old_head, *new_head);
	} while (unlikely(success == 0));
	return n;
}

static __al_inline unsigned int
__usg_ring_move_cons_head(struct usg_ring *r, unsigned int is_sc,
		unsigned int n, enum usg_ring_queue_behavior behavior,
		uint32_t *old_head, uint32_t *new_head,
		uint32_t *entries)
{
	unsigned int max = n;
	int success;

	do {
		n = max;

		*old_head = r->cons.head;

		usg_rmb();

		*entries = (r->prod.tail - *old_head);

		if (n > *entries)
			n = (behavior == USG_RING_QUEUE_FIXED) ? 0 : *entries;

		if (unlikely(n == 0))
			return 0;

		*new_head = *old_head + n;
		if (is_sc)
			r->cons.head = *new_head, success = 1;
		else
			success = usg_atomic32_cmpset(&r->cons.head, *old_head,
					*new_head);
	} while (unlikely(success == 0));
	return n;
}

static __al_inline unsigned int
__usg_ring_do_enqueue(struct usg_ring *r, void * const *obj_table,
		 unsigned int n, enum usg_ring_queue_behavior behavior,
		 unsigned int is_sp, unsigned int *free_space)
{
	uint32_t prod_head, prod_next;
	uint32_t free_entries;

    /* ����r->prod.headָ����� */
	n = __usg_ring_move_prod_head(r, is_sp, n, behavior,
			&prod_head, &prod_next, &free_entries);
	if (n == 0)
		goto end;

    /* r����__usg_ring_move_prod_head�����r->prod.head�Ѿ��ƶ�����Ҫ��λ��
       &r[1]�����ݵ�λ��, prod_head�Ǿɵ�r->prod.head��obj_table��Ҫ�����obj
       ENQUEUE_PTRS�Ĵ���Ŀ���ǰѶ�Ӧ������obj��ŵ�r��ָ��λ�����棬����
       obj��r������Ѿ�վ�ã���������ֻҪ��ָ����伴�ɣ�����Ҫ����
    */
	ENQUEUE_PTRS(r, &r[1], prod_head, obj_table, n, void *);

    /* ����r->prod.tailָ����� */
	update_tail(&r->prod, prod_head, prod_next, is_sp, 1);
end:
	if (free_space != NULL)
		*free_space = free_entries - n;
	return n;
}

static __al_inline unsigned int
__usg_ring_do_dequeue(struct usg_ring *r, void **obj_table,
		 unsigned int n, enum usg_ring_queue_behavior behavior,
		 unsigned int is_sc, unsigned int *available)
{
	uint32_t cons_head, cons_next;
	uint32_t entries;

    /* ����r->cons.head */
	n = __usg_ring_move_cons_head(r, (int)is_sc, n, behavior,
			&cons_head, &cons_next, &entries);
	if (n == 0)
		goto end;

	DEQUEUE_PTRS(r, &r[1], cons_head, obj_table, n, void *);

    /* ����r->cons.tail */
	update_tail(&r->cons, cons_head, cons_next, is_sc, 0);

end:
	if (available != NULL)
		*available = entries - n;
	return n;
}

static __al_inline unsigned int
usg_ring_mp_enqueue_bulk(struct usg_ring *r, void * const *obj_table,
			 unsigned int n, unsigned int *free_space)
{
	return __usg_ring_do_enqueue(r, obj_table, n, USG_RING_QUEUE_FIXED,
			__IS_MP, free_space);
}

static __al_inline unsigned int
usg_ring_sp_enqueue_bulk(struct usg_ring *r, void * const *obj_table,
			 unsigned int n, unsigned int *free_space)
{
	return __usg_ring_do_enqueue(r, obj_table, n, USG_RING_QUEUE_FIXED,
			__IS_SP, free_space);
}

static __al_inline unsigned int
usg_ring_enqueue_bulk(struct usg_ring *r, void * const *obj_table,
		      unsigned int n, unsigned int *free_space)
{
	return __usg_ring_do_enqueue(r, obj_table, n, USG_RING_QUEUE_FIXED,
			r->prod.single, free_space);
}

static __al_inline int
usg_ring_mp_enqueue(struct usg_ring *r, void *obj)
{
	return usg_ring_mp_enqueue_bulk(r, &obj, 1, NULL) ? 0 : -ENOBUFS;
}

static __al_inline int
usg_ring_sp_enqueue(struct usg_ring *r, void *obj)
{
	return usg_ring_sp_enqueue_bulk(r, &obj, 1, NULL) ? 0 : -ENOBUFS;
}

static __al_inline int
usg_ring_enqueue(struct usg_ring *r, void *obj)
{
	return usg_ring_enqueue_bulk(r, &obj, 1, NULL) ? 0 : -ENOBUFS;
}

static __al_inline unsigned int
usg_ring_mc_dequeue_bulk(struct usg_ring *r, void **obj_table,
		unsigned int n, unsigned int *available)
{
	return __usg_ring_do_dequeue(r, obj_table, n, USG_RING_QUEUE_FIXED,
			__IS_MC, available);
}

static __al_inline unsigned int
usg_ring_sc_dequeue_bulk(struct usg_ring *r, void **obj_table,
		unsigned int n, unsigned int *available)
{
	return __usg_ring_do_dequeue(r, obj_table, n, USG_RING_QUEUE_FIXED,
			__IS_SC, available);
}

static __al_inline unsigned int
usg_ring_dequeue_bulk(struct usg_ring *r, void **obj_table, unsigned int n,
		unsigned int *available)
{
	return __usg_ring_do_dequeue(r, obj_table, n, USG_RING_QUEUE_FIXED,
				r->cons.single, available);
}

static __al_inline int
usg_ring_mc_dequeue(struct usg_ring *r, void **obj_p)
{
	return usg_ring_mc_dequeue_bulk(r, obj_p, 1, NULL)  ? 0 : -ENOENT;
}

static __al_inline int
usg_ring_sc_dequeue(struct usg_ring *r, void **obj_p)
{
	return usg_ring_sc_dequeue_bulk(r, obj_p, 1, NULL) ? 0 : -ENOENT;
}

static __al_inline int
usg_ring_dequeue(struct usg_ring *r, void **obj_p)
{
	return usg_ring_dequeue_bulk(r, obj_p, 1, NULL) ? 0 : -ENOENT;
}

static inline unsigned
usg_ring_count(const struct usg_ring *r)
{
	uint32_t prod_tail = r->prod.tail;
	uint32_t cons_tail = r->cons.tail;
	uint32_t count = (prod_tail - cons_tail) & r->mask;
	return (count > r->capacity) ? r->capacity : count;
}

static inline unsigned
usg_ring_free_count(const struct usg_ring *r)
{
	return r->capacity - usg_ring_count(r);
}

static inline int
usg_ring_full(const struct usg_ring *r)
{
	return usg_ring_free_count(r) == 0;
}

static inline int
usg_ring_empty(const struct usg_ring *r)
{
	return usg_ring_count(r) == 0;
}

static inline unsigned int
usg_ring_get_size(const struct usg_ring *r)
{
	return r->size;
}

static inline unsigned int
usg_ring_get_capacity(const struct usg_ring *r)
{
	return r->capacity;
}

static __al_inline unsigned
usg_ring_mp_enqueue_burst(struct usg_ring *r, void * const *obj_table,
			 unsigned int n, unsigned int *free_space)
{
	return __usg_ring_do_enqueue(r, obj_table, n,
			USG_RING_QUEUE_VARIABLE, __IS_MP, free_space);
}

static __al_inline unsigned
usg_ring_sp_enqueue_burst(struct usg_ring *r, void * const *obj_table,
			 unsigned int n, unsigned int *free_space)
{
	return __usg_ring_do_enqueue(r, obj_table, n,
			USG_RING_QUEUE_VARIABLE, __IS_SP, free_space);
}

static __al_inline unsigned
usg_ring_enqueue_burst(struct usg_ring *r, void * const *obj_table,
		      unsigned int n, unsigned int *free_space)
{
	return __usg_ring_do_enqueue(r, obj_table, n, USG_RING_QUEUE_VARIABLE,
			r->prod.single, free_space);
}

static __al_inline unsigned
usg_ring_mc_dequeue_burst(struct usg_ring *r, void **obj_table,
		unsigned int n, unsigned int *available)
{
	return __usg_ring_do_dequeue(r, obj_table, n,
			USG_RING_QUEUE_VARIABLE, __IS_MC, available);
}

static __al_inline unsigned
usg_ring_sc_dequeue_burst(struct usg_ring *r, void **obj_table,
		unsigned int n, unsigned int *available)
{
	return __usg_ring_do_dequeue(r, obj_table, n,
			USG_RING_QUEUE_VARIABLE, __IS_SC, available);
}

static __al_inline unsigned
usg_ring_dequeue_burst(struct usg_ring *r, void **obj_table,
		unsigned int n, unsigned int *available)
{
	return __usg_ring_do_dequeue(r, obj_table, n,
				USG_RING_QUEUE_VARIABLE,
				r->cons.single, available);
}

#ifdef __cplusplus
}
#endif

#endif /* _USG_RING_H_ */

