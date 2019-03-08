#include "usg_common.h"
#include <sys/queue.h>

#include "usg_ring.h"

/**
 * Combines 32b inputs most significant set bits into the least
 * significant bits to construct a value with the same MSBs as x
 * but all 1's under it.
 * MSB�������Чbitλ
 * LSB�������Чbitλ
 * ��һ��32b��ֵ����0bit�������Чbitȫ������Ϊ1
 * @param x
 *    The integer whose MSBs need to be combined with its LSBs
 * @return
 *    The combined value.
 */
static inline uint32_t
usg_combine32ms1b(register uint32_t x)
{
	x |= x >> 1;
	x |= x >> 2;
	x |= x >> 4;
	x |= x >> 8;
	x |= x >> 16;

	return x;
}

/**
 * Aligns input parameter to the next power of 2
 *
 * @param x
 *   The integer value to algin
 *
 * @return
 *   Input parameter aligned to the next power of 2
 */
static inline uint32_t
usg_align32pow2(uint32_t x)
{
    /*
     * ���ȣ�usg_combine32ms1b�õ���ֵ��һ����0bit�������Чbitȫ������Ϊ1��ֵ��
     * ����x = 1010b����õ�x = 1111b
     * �ĺ�����Ŀ����ͨ��x�õ�һ��2��n�η���ֵ
     * ��ôֻҪ��usg_combine32ms1b�õ���ֵ+1��ȿ��Եõ���
     * ���ﲻ����ΪʲôҪx--����������õ�ʱ��x+1,Ϊ�˷�ֹx�����Ϊ0��?
     */
	x--;
	x = usg_combine32ms1b(x);

	return x + 1;
}

/* return the size of memory occupied by a ring */
ssize_t
usg_ring_get_memsize(unsigned count)
{
	ssize_t sz;

	/* count must be a power of 2 */
	if ((!POWEROF2(count)) || (count > USG_RING_SZ_MASK )) {
		return -EINVAL;
	}

	sz = sizeof(struct usg_ring) + count * sizeof(void *);
	sz = USG_ALIGN(sz, USG_CACHE_LINE_SIZE);
	return sz;
}

int
usg_ring_init(struct usg_ring *r, const char *name, unsigned count,
	unsigned flags)
{
	/* compilation-time checks */
	USG_BUILD_BUG_ON((sizeof(struct usg_ring) &
			  USG_CACHE_LINE_MASK) != 0);
	USG_BUILD_BUG_ON((offsetof(struct usg_ring, cons) &
			  USG_CACHE_LINE_MASK) != 0);
	USG_BUILD_BUG_ON((offsetof(struct usg_ring, prod) &
			  USG_CACHE_LINE_MASK) != 0);

	/* init the ring structure */
	memset(r, 0, sizeof(*r));

	r->flags = flags;
	r->prod.single = (flags & RING_F_SP_ENQ) ? __IS_SP : __IS_MP;
	r->cons.single = (flags & RING_F_SC_DEQ) ? __IS_SC : __IS_MC;

	if (flags & RING_F_EXACT_SZ) {
		r->size = usg_align32pow2(count + 1);
		r->mask = r->size - 1;
		r->capacity = count;
	} else {
		if ((!POWEROF2(count)) || (count > USG_RING_SZ_MASK)) {
			return -EINVAL;
		}
		r->size = count;
		r->mask = count - 1;
		r->capacity = r->mask;
	}
	r->prod.head = r->cons.head = 0;
	r->prod.tail = r->cons.tail = 0;

	return 0;
}

/* create the ring */
struct usg_ring *
usg_ring_create(const char *name, unsigned count, int socket_id,
		unsigned flags)
{
	struct usg_ring *r;
	ssize_t ring_size;

    /* �ж�flags��ʾ�Ƿ�������RING_F_EXACT_SZ, RING_F_EXACT_SZ�Ļ����ڲ���
       ��count��һ������������Ϊ2��ָ���η�������Ҫ�������������count�������
       2��ָ���η�*/
	/* for an exact size ring, round up from count to a power of two */
	if (flags & RING_F_EXACT_SZ)
		count = usg_align32pow2(count + 1);

    /* ����count��ȡring��Ҫ�Ĵ�С,
       ring_size�Ĵ�С��������
       ring_size = sizeof(struct usg_ring) + count * sizeof(void *);
       ring_size = USG_ALIGN(ring_size, USG_CACHE_LINE_SIZE);
       ������sizeof(struct usg_ring)+count��ָ��Ĵ�С������ring���������
       ���ݾ���ָ��.
    */
	ring_size = usg_ring_get_memsize(count);
	if (ring_size < 0) {
		return NULL;
	}

    r = malloc(ring_size);
	if (r == NULL) {
		return NULL;
	}

    memset(r, 0, ring_size);
	return r;
}

/* free the ring */
void
usg_ring_free(struct usg_ring *r)
{
	if (r != NULL) {
        free(r);
		return;
	}
}

/* dump the status of the ring on the console */
void
usg_ring_dump(FILE *f, const struct usg_ring *r)
{
	fprintf(f, "  flags=%x\n", r->flags);
	fprintf(f, "  size=%"PRIu32"\n", r->size);
	fprintf(f, "  capacity=%"PRIu32"\n", r->capacity);
	fprintf(f, "  ct=%"PRIu32"\n", r->cons.tail);
	fprintf(f, "  ch=%"PRIu32"\n", r->cons.head);
	fprintf(f, "  pt=%"PRIu32"\n", r->prod.tail);
	fprintf(f, "  ph=%"PRIu32"\n", r->prod.head);
	fprintf(f, "  used=%u\n", usg_ring_count(r));
	fprintf(f, "  avail=%u\n", usg_ring_free_count(r));
}

