#include "rsa.h"
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>
#include <string.h>
#include <math.h>

#define IS_DIGIT(n) ((n)>='0' && (n)<='9')
#define CHAR_2_INT(c) ((int)((c) - '0'))
#define COPRIME_PRIME(X) ((X).prime)
#define COPRIME_DIVISOR(X) ((X).divisor)

#define FIRST

static const u1024_t NUM_0 = {.seg_00=0};
static const u1024_t NUM_1 = {.seg_00=1};
static const u1024_t NUM_2 = {.seg_00=2};
static const u1024_t NUM_5 = {.seg_00=5};
static const u1024_t NUM_10 = {.seg_00=10};

typedef int (* func_modular_multiplication_t) (u1024_t *num_res, 
    u1024_t *num_a, u1024_t *num_b, u1024_t *num_n);

STATIC u1024_t num_montgomery_n, num_montgomery_factor;

void INLINE number_reset(u1024_t *num)
{
    memset(num, 0, sizeof(u1024_t));
}

STATIC void INLINE number_add(u1024_t *res, u1024_t *num1, u1024_t *num2)
{
    static u1024_t tmp_res;
    static u64 *max_advance, cmask;
    u64 *seg = NULL, *seg1 = NULL, *seg2 = NULL, carry = 0;

    TIMER_START(FUNC_NUMBER_ADD);
    if (!max_advance || !cmask)
    {
	/* bit carrying is continues into the buffer u64 to accomodate for  
	 * number_montgomery_product()
	 */
	max_advance = (u64 *)&tmp_res + BLOCK_SZ_U1024 + 1;
	cmask = MSB_PT(u64);
    }

    number_reset(&tmp_res);
    for (seg = (u64 *)&tmp_res, seg1 = (u64 *)num1, seg2 = (u64 *)num2;
	seg < max_advance; seg++, seg1++, seg2++)
    {
	if (!*seg1)
	{
	    *seg = *seg2;
	    if (carry)
	    {
		carry = *seg2 == (u64)-1 ? (u64)1 : (u64)0;
		(*seg)++;
	    }
	    continue;
	}
	if (!*seg2)
	{
	    *seg = *seg1;
	    if (carry)
	    {
		carry = *seg1 == (u64)-1 ? (u64)1 : (u64)0;
		(*seg)++;
	    }
	    continue;
	}
	*seg = *seg1 + *seg2 + carry;
	if ((*seg1 & cmask) && (*seg2 & cmask))
	    carry = 1;
	else if (!(*seg1 & cmask) && !(*seg2 & cmask))
	    carry = 0;
	else
	    carry = (*seg & cmask) ? 0 : 1;
    }
    *res = tmp_res;
    TIMER_STOP(FUNC_NUMBER_ADD);
}

static void INLINE number_shift_right_once(u1024_t *num)
{
    u64 *seg;

    TIMER_START(FUNC_NUMBER_SHIFT_RIGHT_ONCE);
    /* shifting is done from the buffer u64 to accomodate for 
     * number_montgomery_product()
     */
    for (seg = (u64 *)num; seg < (u64 *)num + BLOCK_SZ_U1024; seg++)
    {
	*seg = *seg >> 1;
	*seg = (*(seg+1) & (u64)1) ? *seg | MSB_PT(u64) : *seg & ~MSB_PT(u64);
    }
    *seg = *seg >> 1;
    TIMER_STOP(FUNC_NUMBER_SHIFT_RIGHT_ONCE);
}

static void INLINE number_shift_left_once(u1024_t *num)
{
    u64 *seg;

    TIMER_START(FUNC_NUMBER_SHIFT_LEFT_ONCE);
    for (seg = (u64*)num + BLOCK_SZ_U1024; seg > (u64*)num; seg--)
    {
	*seg = *seg << 1;
	*seg = *(seg-1) & MSB_PT(u64) ? *seg | (u64)1 : *seg & ~(u64)1;
    }
    *seg = *seg << 1;
    TIMER_STOP(FUNC_NUMBER_SHIFT_LEFT_ONCE);
}

int INLINE number_init_random(u1024_t *num, int bit_len)
{
    int i, max, ret;
    struct timeval tv;
    u1024_t num_mask;

    TIMER_START(FUNC_NUMBER_INIT_RANDOM);
    if (bit_len < 1 || bit_len > BIT_SZ_U1024)
	return -1;

    number_reset(num);
    tv.tv_sec = 0;
    tv.tv_usec = 0;
    if (gettimeofday(&tv, NULL))
    {
	ret = -1;
	goto Exit;
    }
    srandom((unsigned int)tv.tv_sec * (unsigned int)tv.tv_usec);

    /* initiate a BIT_SZ_U1024 random number */
    max = (BIT_SZ_U1024/(sizeof(long)<<3));
    for (i = 0; i < max; i++)
	*((long *)num + i) |= random();

    /* create bit mask according to bit_len */
    number_reset(&num_mask);
    for (i = 0; i < bit_len; i++)
    {
	u1024_t num_1 = NUM_1;

	number_shift_left_once(&num_mask);
	number_add(&num_mask, &num_mask, &num_1);
    }
    /* apply num_mask to num */
    for (i = 0; i < BLOCK_SZ_U1024; i++)
	*((u64 *)num + i) &= *((u64*)&num_mask + i);

    ret = 0;

Exit:
    TIMER_STOP(FUNC_NUMBER_INIT_RANDOM);
    return ret;
}

STATIC int INLINE number_find_most_significant_set_bit(u1024_t *num, 
    u64 **major, u64 *minor)
{
    u64 *tmp_major = (u64 *)num + BLOCK_SZ_U1024 - 1;
    u64 tmp_minor;
    int minor_offset;

    TIMER_START(FUNC_NUMBER_FIND_MOST_SIGNIFICANT_SET_BIT);
    while (tmp_major >= (u64 *)num)
    {
	tmp_minor = MSB_PT(u64);
	minor_offset = BIT_SZ_U64;

	while (tmp_minor)
	{
	    if (!(*tmp_major & tmp_minor))
	    {
		tmp_minor = tmp_minor >> 1;
		minor_offset--;
		continue;
	    }
	    goto Exit;
	}
	tmp_major--;
    }
    tmp_major++; /* while loop terminates when tmp_major == (u64 *)num - 1 */

Exit:
    *minor = tmp_minor;
    *major = tmp_major;

    TIMER_STOP(FUNC_NUMBER_FIND_MOST_SIGNIFICANT_SET_BIT);
    return minor_offset;
}

STATIC void INLINE number_small_dec2num(u1024_t *num_n, u64 dec)
{
    u64 zero = (u64)0;
    u64 *ptr = &zero;

    TIMER_START(FUNC_NUMBER_SMALL_DEC2NUM);
    number_reset(num_n);
    *(u64 *)num_n = (u64)(*ptr | dec);
    TIMER_STOP(FUNC_NUMBER_SMALL_DEC2NUM);
}

STATIC void INLINE number_2complement(u1024_t *res, u1024_t *num)
{
    u1024_t tmp, num_1;
    u64 *seg = NULL;

    TIMER_START(FUNC_NUMBER_2COMPLEMENT);
    num_1 = NUM_1;
    tmp = *num;
    for (seg = (u64 *)&tmp; 
	seg - (u64 *)&tmp <= BLOCK_SZ_U1024; seg++)
    {
	*seg = ~*seg; /* one's complement */
    }

    number_add(res, &tmp, &num_1); /* two's complement */
    TIMER_STOP(FUNC_NUMBER_2COMPLEMENT);
}

STATIC void INLINE number_sub(u1024_t *res, u1024_t *num1, u1024_t *num2)
{
    u1024_t num2_2complement;

    TIMER_START(FUNC_NUMBER_SUB);
    number_2complement(&num2_2complement, num2);
    number_add(res, num1, &num2_2complement);
    *((u64 *)res + BLOCK_SZ_U1024) = 0;
    TIMER_STOP(FUNC_NUMBER_SUB);
}

void number_sub1(u1024_t *num)
{
    u1024_t num_1;

    num_1 = NUM_1;
    number_sub(num, num, &num_1);
}

void INLINE number_mul(u1024_t *res, u1024_t *num1, u1024_t *num2)
{
    int i;
    u1024_t tmp_res, multiplicand = *num1, multiplier = *num2;

    TIMER_START(FUNC_NUMBER_MUL);
    number_reset(&tmp_res);
    for (i = 0; i < BLOCK_SZ_U1024; i++)
    {
	u64 mask = 1;
	int j;

	for (j = 0; j < BIT_SZ_U64; j++)
	{
	    if ((*((u64 *)(&multiplier) + i)) & mask)
		number_add(&tmp_res, &tmp_res, &multiplicand);
	    number_shift_left_once(&multiplicand);
	    *((u64 *)&multiplicand + BLOCK_SZ_U1024) = 0;
	    mask = mask << 1;
	}
    }
    *res = tmp_res;
    TIMER_STOP(FUNC_NUMBER_MUL);
}

STATIC void INLINE number_absolute_value(u1024_t *abs, u1024_t *num)
{
    TIMER_START(FUNC_NUMBER_ABSOLUTE_VALUE);
    *abs = *num;
    if (NUMBER_IS_NEGATIVE(num))
    {
	u1024_t num_1 = NUM_1;
	u64 *seg;

	number_sub(abs, abs, &num_1);
	for (seg = (u64 *)abs + BLOCK_SZ_U1024 - 1; seg >= (u64 *)abs; seg--)
	    *seg = ~*seg;
    }
    TIMER_STOP(FUNC_NUMBER_ABSOLUTE_VALUE);
}

/* return: num1 > num2  or ret_on_equal if num1 == num2 */
static int INLINE number_compare(u1024_t *num1, u1024_t *num2, int ret_on_equal)
{
    int ret;
    u64 *seg1 = (u64 *)num1 + BLOCK_SZ_U1024;
    u64 *seg2 = (u64 *)num2 + BLOCK_SZ_U1024;

    TIMER_START(FUNC_NUMBER_COMPARE);
    for (; seg1 > (u64 *)num1 && *seg1==*seg2; seg1--, seg2--);
    ret = *seg1==*seg2 ? ret_on_equal : *seg1>*seg2;
    TIMER_STOP(FUNC_NUMBER_COMPARE);
    return ret;
}

/* return: num1 > num2 */
STATIC int INLINE number_is_greater(u1024_t *num1, u1024_t *num2)
{
    int ret;

    TIMER_START(FUNC_NUMBER_IS_GREATER);
    ret = number_compare(num1, num2, 0);
    TIMER_STOP(FUNC_NUMBER_IS_GREATER);
    return ret;
}

/* return: num1 >= num2 */
STATIC int INLINE number_is_greater_or_equal(u1024_t *num1, u1024_t *num2)
{
    int ret;

    TIMER_START(FUNC_NUMBER_IS_GREATER_OR_EQUAL);
    ret = number_compare(num1, num2, 1);
    TIMER_STOP(FUNC_NUMBER_IS_GREATER_OR_EQUAL);
    return ret;
}

/* return: num1 == num2 */
STATIC int INLINE number_is_equal(u1024_t *num1, u1024_t *num2)
{
    return !memcmp(num1, num2, BIT_SZ_U1024>>3);
}

STATIC void INLINE number_dev(u1024_t *num_q, u1024_t *num_r, 
    u1024_t *num_dividend, u1024_t *num_divisor)
{
    u1024_t dividend = *num_dividend, divisor = *num_divisor, quotient, 
	remainder;
    u64 *seg_dividend = (u64 *)&dividend + BLOCK_SZ_U1024 - 1;
    u64 *remainder_ptr = (u64 *)&remainder, *quotient_ptr = (u64 *)&quotient;

    TIMER_START(FUNC_NUMBER_DEV);
    number_reset(&remainder);
    number_reset(&quotient);
    while (seg_dividend >= (u64 *)&dividend)
    {
	u64 mask_dividend = MSB_PT(u64);

	while (mask_dividend)
	{
	    number_shift_left_once(&remainder);
	    *((u64 *)&remainder + BLOCK_SZ_U1024) = 0;
	    number_shift_left_once(&quotient);
	    *((u64 *)&quotient + BLOCK_SZ_U1024) = 0;
	    *remainder_ptr = *remainder_ptr |
		((*seg_dividend & mask_dividend) ? (u64)1 : (u64)0);
	    if (number_is_greater_or_equal(&remainder, &divisor))
	    {
		*quotient_ptr = *quotient_ptr | (u64)1;
		number_sub(&remainder, &remainder, &divisor);
	    }
	    mask_dividend = mask_dividend >> 1;
	}
	seg_dividend--;
    }
    *num_q = quotient;
    *num_r = remainder;
    TIMER_STOP(FUNC_NUMBER_DEV);
}

STATIC void INLINE number_mod(u1024_t *r, u1024_t *a, u1024_t *n)
{
    u1024_t q;

    TIMER_START(FUNC_NUMBER_MOD);
    number_dev(&q, r, a, n);
    TIMER_STOP(FUNC_NUMBER_MOD);
}

STATIC int INLINE number_modular_multiplication_naive(u1024_t *num_res, 
    u1024_t *num_a, u1024_t *num_b, u1024_t *num_n)
{
    u1024_t tmp;

    TIMER_START(FUNC_NUMBER_MODULAR_MULTIPLICATION_NAIVE);
    number_mul(&tmp, num_a, num_b);
    number_mod(num_res, &tmp, num_n);
    *((u64 *)num_res + BLOCK_SZ_U1024) = 0;
    TIMER_STOP(FUNC_NUMBER_MODULAR_MULTIPLICATION_NAIVE);
    return 0;
}

/* assigns num_n: 0 < num_n < range */
static void INLINE number_init_random_strict_range(u1024_t *num_n, 
    u1024_t *range)
{
    u1024_t num_tmp, num_range_min1, num_1;

    TIMER_START(FUNC_NUMBER_INIT_RANDOM_STRICT_RANGE);
    num_1 = NUM_1;
    number_sub(&num_range_min1, range, &num_1);
    number_init_random(&num_tmp, BIT_SZ_U1024);
    number_mod(&num_tmp, &num_tmp, &num_range_min1);
    number_add(&num_tmp, &num_tmp, &num_1);

    *num_n = num_tmp;
    TIMER_STOP(FUNC_NUMBER_INIT_RANDOM_STRICT_RANGE);
}

STATIC void INLINE number_exponentiation(u1024_t *res, u1024_t *num_base, 
    u1024_t *num_exp)
{
    u1024_t num_cnt, num_1, num_tmp;

    TIMER_START(FUNC_NUMBER_EXPONENTIATION);
    num_1 = NUM_1;
    num_cnt = NUM_0;
    num_tmp = NUM_1;

    while (!number_is_equal(&num_cnt, num_exp))
    {
	number_mul(&num_tmp, &num_tmp, num_base);
	number_add(&num_cnt, &num_cnt, &num_1);
    }

    *res = num_tmp;
    TIMER_STOP(FUNC_NUMBER_EXPONENTIATION);
}

STATIC int INLINE number_modular_exponentiation_naive(u1024_t *res, u1024_t *a, 
    u1024_t *b, u1024_t *n)
{
    u1024_t d, num_1;
    u64 *seg = NULL, mask;

    TIMER_START(FUNC_NUMBER_MODULAR_EXPONENTIATION_NAIVE);
    d = NUM_1;
    num_1 = NUM_1;
    number_find_most_significant_set_bit(b, &seg, &mask);
    while (seg >= (u64 *)b)
    {
	while (mask)
	{
	    if (number_modular_multiplication_naive(&d, &d, &d, n))
		return -1;
	    if (*seg & mask)
	    {
		if (number_modular_multiplication_naive(&d, &d, a, n))
		    return -1;
	    }

	    mask = mask >> 1;
	}
	mask = MSB_PT(u64);
	seg--;
    }
    *res = d;
    TIMER_STOP(FUNC_NUMBER_MODULAR_EXPONENTIATION_NAIVE);
    return 0;
}

STATIC int INLINE number_gcd_is_1(u1024_t *num_a, u1024_t *num_b)
{
    /* algorithm
     * ---------
     * g = 0
     * while u is even and v is even
     *   u = u/2 (right shift)
     *   v = v/2
     *   g = g + 1
     * now u or v (or both) are odd
     * while u > 0
     *   if u is even, u = u/2
     *   else if v is even, v = v/2
     *   else if u >= v
     *     u = (u-v)/2
     *   else
     *     v = (v-u)/2
     * return v/2^k
     */

    /* Since radix is of the form 2^k, and n is odd, their GCD is 1 */
    return 1;
}

STATIC inline int number_is_odd(u1024_t *num)
{
    int ret;

    TIMER_START(FUNC_NUMBER_IS_ODD);
    ret = *(u64 *)num & (u64)1;
    TIMER_STOP(FUNC_NUMBER_IS_ODD);
    return ret;
}

/* montgomery product
 * MonPro(a, b, n)
 *   s(-1) = 0
 *   a = 2a
 *   for i = 0 to n do
 *     q(i) = s(i-1) mod 2 (LSB of s(i-1)
 *     s(i) = (s(i-1) + q(i)n + b(i)a)/2
 *   end for
 *   return s(n)
 */
static void INLINE number_montgomery_product(u1024_t *num_res, u1024_t *num_a, 
    u1024_t *num_b, u1024_t *num_n)
{
    u1024_t multiplier = *num_a, num_s;
    u64 *seg = NULL;
    int i;

    TIMER_START(FUNC_NUMBER_MONTGOMERY_PRODUCT);
    num_s = NUM_0;
    number_shift_left_once(&multiplier);

    /* handle the first BIT_SZ_U1024 iterations */
    for (seg = (u64 *)num_b; seg < (u64 *)num_b + BLOCK_SZ_U1024; seg++)
    {
	u64 mask;

	for (mask = (u64)1; mask; mask = mask<<1)
	{
	    if (number_is_odd(&num_s))
		number_add(&num_s, &num_s, num_n);
	    if (*seg & mask)
		number_add(&num_s, &num_s, &multiplier);
	    number_shift_right_once(&num_s);
	}
    }

    /* handle extra 2 iterations, as buffer size is is considered to be 
     * MAX(bit_sz) + 2.
     */
    for (i = 0 ;i < 3; i++)
    {
	if (number_is_odd(&num_s))
	    number_add(&num_s, &num_s, num_n);
	/* the two overflow bits of num_b are zero */
	number_shift_right_once(&num_s);
    }

    *num_res = num_s;
    TIMER_STOP(FUNC_NUMBER_MONTGOMERY_PRODUCT);
}

/* shift left and do mod num_n 2*(BIT_SZ_U1024+2) times... */
void INLINE number_montgomery_factor_set(u1024_t *num_n, u1024_t *num_factor)
{
    u1024_t num_0, factor;
    int exp, exp_max;

    TIMER_START(FUNC_NUMBER_MONTGOMERY_FACTOR_SET);
    if (number_is_equal(&num_montgomery_n, num_n))
	return;

    if (num_factor)
    {
	num_montgomery_factor = *num_factor;
	return;
    }

    exp_max = 2*(BIT_SZ_U1024+2);
    num_0 = NUM_0;
    number_small_dec2num(&factor, (u64)1);
    exp = 0;

    while (exp < exp_max)
    {
	while (!factor.buffer && number_is_greater(num_n, &factor))
	{
	    if (exp == exp_max)
		goto Exit;
	    number_shift_left_once(&factor);
	    exp++;
	}
	number_sub(&factor, &factor, num_n);
    }

Exit:
    num_montgomery_factor = factor;
    TIMER_START(FUNC_NUMBER_MONTGOMERY_FACTOR_SET);
}

/* a: exponent
 * b: power
 * n: modulus
 * r: 2^(BIT_SZ_U1024)%n
 * MonPro(a, b, n) = abr^-1%n
 *
 * a * b % n = abrr^-1%n = 1abrr^-1%n = MonPro(1, abr%n, n) = 
 *             MonPro(1, arbrr^-1%n, n) = MonPro(1, ar%n*br%n*r^-1, n) =
 *             MonPro(1, a(r^2)(r^-1)%n * b(r^2)(r^-1) * (r^-1), n) =
 *             MonPro(1, MonPro(a(r^2)(r^-1)%n, b(r^2)(r^-1), n), n) =
 *             MonPro(1, MonPro(MonPro(a, r^2%n, n), MonPro(b, r^2%n, n), n), n)
 *
 * num_montgomery_factor = r^2%n = 2^2BIT_SZ(u1024_t)%n
 * a_tmp = MonPro(a, r^2%n, n)
 * b_tmp = MonPro(b, r^2%n, n)
 * a * b % n = MonPro(1, MonPro(a_tmp, b_tmp, n), n)
 */
STATIC int INLINE number_modular_multiplication_montgomery(u1024_t *num_res, 
    u1024_t *num_a, u1024_t *num_b, u1024_t *num_n)
{
    int ret;
    u1024_t a_tmp, b_tmp, num_1;

    TIMER_START(FUNC_NUMBER_MODULAR_MULTIPLICATION_MONTGOMERY);
    number_montgomery_factor_set(num_n, NULL);

    num_1 = NUM_1;
    number_montgomery_product(&a_tmp, num_a, &num_montgomery_factor, num_n);
    number_montgomery_product(&b_tmp, num_b, &num_montgomery_factor, num_n);
    number_montgomery_product(num_res, &a_tmp, &b_tmp, num_n);
    number_montgomery_product(num_res, &num_1, num_res, num_n);
    ret = 0;

    TIMER_STOP(FUNC_NUMBER_MODULAR_MULTIPLICATION_MONTGOMERY);
    return ret;
}

/* montgomery (right-left, speed optimised) modular exponentiation procedure:
 * MonExp(a, b, n)
 *   c = 2^(2n)
 *   A = MonPro(c, a, n) (mapping)
 *   r = MonPro(c, 1, n)
 *   for i = 0 to k-1 do
 *     if (bi==1) then
 *       r = MonPro(r, a, n) (multiply)
 *     end if
 *     A = MonPro(A, A, n) (square)
 *   end for
 *   r = MonPro(1, r, n)
 *   return r
 */
STATIC int INLINE number_modular_exponentiation_montgomery(u1024_t *res, 
    u1024_t *a, u1024_t *b, u1024_t *n)
{
    u1024_t a_nresidue, res_nresidue, num_1;
    u64 *seg;
    int ret = 0;

    TIMER_START(FUNC_NUMBER_MODULAR_EXPONENTIATION_MONTGOMERY);
    num_1 = NUM_1;

    number_montgomery_factor_set(n, NULL);
    number_montgomery_product(&a_nresidue, &num_montgomery_factor, a, n);
    number_montgomery_product(&res_nresidue, &num_montgomery_factor, &num_1, n);

    for (seg = (u64 *)b; seg < (u64 *)b + BLOCK_SZ_U1024; seg++)
    {
	u64 mask;

	for (mask = (u64)1; mask; mask = mask << 1)
	{
	    if (*seg & mask)
	    {
		number_montgomery_product(&res_nresidue, &res_nresidue, 
		    &a_nresidue, n);
	    }
	    number_montgomery_product(&a_nresidue, &a_nresidue, &a_nresidue, n);
	}
    }
    number_montgomery_product(res, &num_1, &res_nresidue, n);

    TIMER_STOP(FUNC_NUMBER_MODULAR_EXPONENTIATION_MONTGOMERY);
    return ret;
}

static void INLINE number_witness_init(u1024_t *num_n_min1, u1024_t *num_u, 
    int *t)
{
    u1024_t tmp = *num_n_min1;

    TIMER_START(FUNC_NUMBER_WITNESS_INIT);
    *t = 0;
    while (!number_is_odd(&tmp))
    {
	number_shift_right_once(&tmp);
	(*t)++;
    }

    *num_u = tmp;
    TIMER_STOP(FUNC_NUMBER_WITNESS_INIT);
}

/* witness method used by the miller-rabin algorithm. attempt to use num_a as a
 * witness of num_n's compositness:
 * if number_witness(num_a, num_n) is true, then num_n is composit
 */
STATIC int INLINE number_witness(u1024_t *num_a, u1024_t *num_n)
{
    u1024_t num_1, num_2, num_u, num_x_prev, num_x_curr, num_n_min1;
    int i, t, ret;

    TIMER_START(FUNC_NUMBER_WITNESS);
    if (!number_is_odd(num_n))
    {
	ret = 1;
	goto Exit;
    }

    num_1 = NUM_1;
    num_2 = NUM_2;
    number_sub(&num_n_min1, num_n, &num_1);
    number_witness_init(&num_n_min1, &num_u, &t);
    if (number_modular_exponentiation_montgomery(&num_x_prev, num_a, &num_u, 
	num_n))
    {
	ret = 1;
	goto Exit;
    }

    for (i = 0; i < t; i++)
    {
	if (number_modular_multiplication_montgomery(&num_x_curr, &num_x_prev, 
	    &num_x_prev, num_n))
	{
	    ret = 1;
	    goto Exit;
	}
	if (number_is_equal(&num_x_curr, &num_1) && 
	    !number_is_equal(&num_x_prev, &num_1) &&
	    !number_is_equal(&num_x_prev, &num_n_min1))
	{
	    ret = 1;
	    goto Exit;
	}
	num_x_prev = num_x_curr;
    }

    if (!number_is_equal(&num_x_curr, &num_1))
    {
	ret = 1;
	goto Exit;
    }
    ret = 0;

Exit:
    TIMER_STOP(FUNC_NUMBER_WITNESS);
    return ret;
}

/* miller-rabin algorithm
 * num_n is an odd integer greater than 2 
 * return:
 * 0 - if num_n is composit
 * 1 - if num_n is almost surely prime
 */
STATIC int INLINE number_miller_rabin(u1024_t *num_n, u1024_t *num_s)
{
    int ret;
    u1024_t num_j, num_a, num_1;

    TIMER_START(FUNC_NUMBER_MILLER_RABIN);
    num_1 = NUM_1;
    num_j = NUM_1;

    while (!number_is_equal(&num_j, num_s))
    {
	number_init_random_strict_range(&num_a, num_n);
	if (number_witness(&num_a, num_n))
	{
	    ret = 0;
	    goto Exit;
	}
	number_add(&num_j, &num_j, &num_1);
    }
    ret = 1;

Exit:
    TIMER_STOP(FUNC_NUMBER_MILLER_RABIN);
    return ret;
}

STATIC int INLINE number_is_prime(u1024_t *num_n)
{
    int ret;
    u1024_t num_s;

    TIMER_START(FUNC_NUMBER_IS_PRIME);
    number_small_dec2num(&num_s, (u64)10);
    ret = number_miller_rabin(num_n, &num_s);

    TIMER_STOP(FUNC_NUMBER_IS_PRIME);
    return ret;
}

/* initiate number_generate_coprime:small_primes[] fields and generate pi and
 * incrementor
 */
static void INLINE number_small_prime_init(small_prime_entry_t *entry, 
    u64 exp_initializer, u1024_t *num_pi, u1024_t *num_increment)
{
    TIMER_START(FUNC_NUMBER_SMALL_PRIME_INIT);

    /* initiate the entry's prime */
    number_small_dec2num(&(entry->prime), entry->prime_initializer);

    /* initiate the entry's exponent */
    number_small_dec2num(&(entry->exp), exp_initializer);

    /* rase the entry's prime to the required power */
    number_exponentiation(&(entry->power_of_prime), &(entry->prime), 
	&(entry->exp));

    /* update pi */
    number_mul(num_pi, num_pi, &(entry->power_of_prime));

    /* update incrementor */
    number_mul(num_increment, num_increment, &(entry->prime));

    TIMER_STOP(FUNC_NUMBER_SMALL_PRIME_INIT);
}

/* num_increment = 304250263527210, is the product of the first 13 primes
 * num_pi = 7.4619233495664116883370964193144e+153, is the product of the first
 *   13 primes raised to the respective power, exp, in small_primes[]. it is a 
 *   512 bit number
 * retuned value: num_coprime is a large number such that 
 *   gcd(num_coprime, num_increment) == 1, that is, it does not devided by any 
 *   of the first 13 primes
 */
STATIC void INLINE number_generate_coprime(u1024_t *num_coprime, 
    u1024_t *num_increment)
{
    int i;
    static u1024_t num_0, num_pi, num_mod, num_jumper, num_inc;
    static int init;
    static small_prime_entry_t small_primes[] = {
	{2}, {3}, {5}, {7}, {11}, {13}, {17}, {19}, {23}, {29}, {31}, {37}, {41}
    };

#ifdef TESTS
    if (init_reset)
    {
	init = 0;
	init_reset = 0;
    }
#endif

    TIMER_START(FUNC_NUMBER_GENERATE_COPRIME);
    if (!init)
    {
	u64 exp_initializer[] = {
#if ENC_LEVEL(128)
	    /* 16353755914851064710 */
	    1, 2, 1, 1, 1, 1, 1, 2, 2, 1, 1, 1, 2
#elif ENC_LEVEL(256)
	    /* 3.310090638572971097793164988204e+38 */
	    3, 3, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3
#elif ENC_LEVEL(512)
	    /* 1.1469339122146834228518724332952e+77 */
	    5, 5, 5, 5, 5, 5, 5, 5, 5, 6, 5, 6, 6
#elif ENC_LEVEL(1024)
	    /* 7.4619233495664116883370964193144e+153 */
	    10, 10, 11, 11, 10, 10, 10, 10, 11, 11, 11, 11, 11
#endif
	};

	/* initiate prime, exp and power_of_prime fields in all small_primes[] 
	 * elements. generate num_inc and num_pi at the same time.
	 */
	num_0 = NUM_0;
	num_pi = num_inc = NUM_1;
	for (i = 0; i < ARRAY_SZ(small_primes); i++)
	{
	    number_small_prime_init(&small_primes[i], exp_initializer[i], 
		&num_pi, &num_inc);
	}

	init = 1;
    }

    /* generate num_coprime, such that gcd(num_coprime, num_increment) == 1 */
    *num_increment = num_inc;
    *num_coprime = NUM_0;
    for (i = 0; i < ARRAY_SZ(small_primes); i++)
    {
	u1024_t num_a, num_a_pow;

	do
	{
	    number_init_random(&num_a, BIT_SZ_U1024/2);
	    number_modular_exponentiation_naive(&num_a_pow, &num_a,
		&(small_primes[i].exp), &num_pi);
	}
	while (number_is_equal(&num_a_pow, &num_0));
	number_add(num_coprime, num_coprime, &num_a);
    }

    /* bound num_coprime to be less than num_pi */
    number_mod(num_coprime, num_coprime, &num_pi);

    /* refine num_coprime:
     * if num_coprime % small_primes[i].prime == 0, then
     * - generate from num_inc, num_jumper, such that 
     *   gcd(num_jumper, small_primes[i].prime) == 1
     * - do: num_coprime = num_coprime + num_jumper
     * thus, gcd(num_coprime, small_primes[i].prime) == 1
     */
    num_jumper = num_inc;
    for (i = 0; i < ARRAY_SZ(small_primes); i++)
    {
	number_mod(&num_mod, num_coprime, &(small_primes[i].prime));
	if (number_is_equal(&num_mod, &num_0))
	{
	    number_dev(&num_jumper, &num_0, &num_jumper, 
		&(small_primes[i].prime));
	}
    }
    if (!number_is_equal(&num_jumper, &num_inc))
	number_add(num_coprime, num_coprime, &num_jumper);
    TIMER_STOP(FUNC_NUMBER_GENERATE_COPRIME);
}

/* determin x, y and gcd according to a and b such that:
 * ax+by == gcd(a, b)
 * NOTE: a is assumed to be >= b */
STATIC void INLINE number_extended_euclid_gcd(u1024_t *gcd, u1024_t *x, 
    u1024_t *a, u1024_t *y, u1024_t *b)
{
    u1024_t num_x, num_x1, num_x2, num_y, num_y1, num_y2, num_0;
    u1024_t num_a, num_b, num_q, num_r;
    int change;

    TIMER_START(FUNC_NUMBER_EXTENDED_EUCLID_GCD);
    if (number_is_greater_or_equal(a, b))
    {
	num_a = *a;
	num_b = *b;
	change = 0;
    }
    else
    {
	num_a = *b;
	num_b = *a;
	change = 1;
    }

    num_x1 = NUM_0;
    num_x2 = NUM_1;
    num_y1 = NUM_1;
    num_y2 = NUM_0;
    num_0 = NUM_0;

    while (number_is_greater(&num_b, &num_0))
    {
	number_dev(&num_q, &num_r, &num_a, &num_b);

	number_mul(&num_x, &num_x1, &num_q);
	number_sub(&num_x, &num_x2, &num_x);
	number_mul(&num_y, &num_y1, &num_q);
	number_sub(&num_y, &num_y2, &num_y);

	num_a = num_b;
	num_b = num_r;
	num_x2 = num_x1;
	num_x1 = num_x;
	num_y2 = num_y1;
	num_y1 = num_y;
    }

    *x = change ? num_y2 : num_x2;
    *y = change ? num_x2 : num_y2;
    *gcd = change ? num_b : num_a;
    TIMER_STOP(FUNC_NUMBER_EXTENDED_EUCLID_GCD);
}

STATIC void INLINE number_euclid_gcd(u1024_t *gcd, u1024_t *a, u1024_t *b)
{
    u1024_t x, y;

    TIMER_START(FUNC_NUMBER_EUCLID_GCD);
    if (number_is_greater_or_equal(a, b))
	number_extended_euclid_gcd(gcd, &x, a, &y, b);
    else
	number_extended_euclid_gcd(gcd, &y, b, &x, a);
    TIMER_STOP(FUNC_NUMBER_EUCLID_GCD);
}

void number_init_random_coprime(u1024_t *num, u1024_t *coprime)
{
    u1024_t num_1, num_gcd;

    TIMER_START(FUNC_NUMBER_INIT_RANDOM_COPRIME);
    num_1 = NUM_1;
    do
    {
	number_init_random_strict_range(num, coprime);
	number_euclid_gcd(&num_gcd, num, coprime);
    }
    while (!number_is_equal(&num_gcd, &num_1));
    TIMER_STOP(FUNC_NUMBER_INIT_RANDOM_COPRIME);
}

/* assumtion: 0 < num < mod */
void number_modular_multiplicative_inverse(u1024_t *inv, u1024_t *num, 
    u1024_t *mod)
{
    u1024_t num_x, num_y, num_gcd, num_y_abs;

    TIMER_START(FUNC_NUMBER_MODULAR_MULTIPLICATIVE_INVERSE);
    number_extended_euclid_gcd(&num_gcd, &num_x, mod, &num_y, num);
    number_absolute_value(&num_y_abs, &num_y);
    number_mod(inv, &num_y_abs, mod);

    if (!number_is_equal(&num_y_abs, &num_y))
	number_sub(inv, mod, inv);
    TIMER_STOP(FUNC_NUMBER_MODULAR_MULTIPLICATIVE_INVERSE);
}

void number_find_prime(u1024_t *num)
{
    u1024_t num_candidate, num_1, num_increment;

    TIMER_START(FUNC_NUMBER_FIND_PRIME);
    num_1 = NUM_1;
    number_generate_coprime(&num_candidate, &num_increment);

    while (!(number_is_prime(&num_candidate)))
    {
	number_add(&num_candidate, &num_candidate, &num_increment);

	/* highly unlikely event of rollover renderring num_candidate == 1 */
	if (number_is_equal(&num_candidate, &num_1))
	    number_generate_coprime(&num_candidate, &num_increment);
    }

    *num = num_candidate;
    TIMER_STOP(FUNC_NUMBER_FIND_PRIME);
}

#ifdef TESTS
STATIC void number_shift_right(u1024_t *num, int n)
{
    int i;

    TIMER_START(FUNC_NUMBER_SHIFT_RIGHT);
    for (i = 0; i < n; i++)
	number_shift_right_once(num);
    TIMER_STOP(FUNC_NUMBER_SHIFT_RIGHT);
}

STATIC void number_shift_left(u1024_t *num, int n)
{
    int i;

    TIMER_START(FUNC_NUMBER_SHIFT_LEFT);
    for (i = 0; i < n; i++)
	number_shift_left_once(num);
    TIMER_STOP(FUNC_NUMBER_SHIFT_LEFT);
}

static u64 *number_get_seg(u1024_t *num, int seg)
{
    u64 *ret;

    if (!num)
	return NULL;

    ret = (u64 *)num + seg;
    return ret;
}

static int is_valid_number_str_sz(char *str)
{
    int ret;

    if (!strlen(str))
    {
	ret = 0;
	goto Exit;
    }

    if (strlen(str) > BIT_SZ_U1024)
    {
	char *ptr = NULL;

	for (ptr = str + strlen(str) - BIT_SZ_U1024 - 1 ;
	    ptr >= str; ptr--)
	{
	    if (*ptr == '1')
	    {
		ret = 0;
		goto Exit;
	    }
	}
    }
    ret = 1;

Exit:
    return ret;

}

int number_init_str(u1024_t *num, char *init_str)
{
    char *ptr = NULL;
    char *end = init_str + strlen(init_str) - 1; /* LSB */
    u64 mask = 1;

    if (!is_valid_number_str_sz(init_str))
	return -1;

    number_reset(num);
    for (ptr = end; ptr >= init_str; ptr--) /* LSB to MSB */
    {
	u64 *seg = NULL;

	if (*ptr != '0' && *ptr != '1')
	    return -1;

	seg = number_get_seg(num, (end - ptr) / BIT_SZ_U64);
	if (*ptr == '1')
	    *seg = *seg | mask;
	mask = (u64)(mask << 1) ? (u64)(mask << 1) : 1;
    }

    return 0;
}

int number_dec2bin(u1024_t *num_bin, char *str_dec)
{
    int ret;
    char *str_start = NULL, *str_end = NULL;
    u1024_t num_counter, num_x10, num_digit, num_addition;
    static char *str_dec2bin[] = {
	"0000", /* 0 */
	"0001", /* 1 */
	"0010", /* 2 */
	"0011", /* 3 */
	"0100", /* 4 */
	"0101", /* 5 */
	"0110", /* 6 */
	"0111", /* 7 */
	"1000", /* 8 */
	"1001", /* 9 */
    };
    
    if (!num_bin || !str_dec)
    {
	ret = -1;
	goto Exit;
    }

    str_start = str_dec;
    str_end = str_dec + strlen(str_dec) - 1;
    number_reset(num_bin);
    number_init_str(&num_x10, "1010");
    number_init_str(&num_counter, "1");
    while (str_start && *str_start == '0')
	str_start++;

    if (str_end < str_start)
    {
	ret = 0;
	goto Exit;
    }

    while (str_start <= str_end) 
    {
	if (!IS_DIGIT(*str_end))
	{
	    ret = -1;
	    goto Exit;
	}

	number_init_str(&num_digit, str_dec2bin[CHAR_2_INT(*str_end)]);
	number_mul(&num_addition, &num_digit, &num_counter);
	number_add(num_bin, num_bin, &num_addition);
	number_mul(&num_counter, &num_counter, &num_x10);
	str_end--;
    }
    ret = 0;

Exit:
    return ret;
}
#endif

