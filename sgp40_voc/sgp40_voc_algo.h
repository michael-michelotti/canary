/* SPDX-License-Identifier: GPL-2.0+ AND BSD-3-Clause */
/*
 * sgp40_voc_algo.h - Sensirion VOC Index Algorithm, kernel port.
 *
 * State struct, API, and Q16.16 fixed-point primitives for the port in
 * sgp40_voc_algo.c. The primitives (fix16_t, F16, fix16_mul, fix16_div,
 * fix16_sqrt, fix16_exp, FIX16_* constants) are copied verbatim from
 * Sensirion's gas-index-algorithm library
 *   https://github.com/Sensirion/gas-index-algorithm
 *   sensirion_gas_index_algorithm_fixpoint/sensirion_gas_index_algorithm.{c,h}
 * Copyright (c) 2022, Sensirion AG. All rights reserved. BSD-3-Clause.
 * The fixed-point core was itself derived from libfixmath
 * (https://github.com/PetteriAimonen/libfixmath, MIT-licensed).
 *
 * These primitives are pure integer arithmetic — no libm, no FPU state —
 * and are therefore safe to call from kernel context. The F16() macro
 * performs a floating-point multiplication, but is only ever invoked on
 * compile-time-constant arguments; under -O2 the compiler folds every
 * F16() call to an integer constant, leaving the resulting object file
 * free of floating-point references.
 */

#ifndef SGP40_VOC_ALGO_H_
#define SGP40_VOC_ALGO_H_

#include <linux/types.h>

typedef int32_t fix16_t;

#define FIX16_MAXIMUM  0x7FFFFFFF
#define FIX16_MINIMUM  0x80000000
#define FIX16_OVERFLOW 0x80000000
#define FIX16_ONE      0x00010000

#define F16(x) \
	((fix16_t)(((x) >= 0) ? ((x) * 65536.0 + 0.5) : ((x) * 65536.0 - 0.5)))

static inline fix16_t fix16_from_int(int32_t a)
{
	return a * FIX16_ONE;
}

static inline int32_t fix16_cast_to_int(fix16_t a)
{
	return (a >= 0) ? (a >> 16) : -((-a) >> 16);
}

static inline fix16_t fix16_mul(fix16_t inArg0, fix16_t inArg1)
{
	uint32_t absArg0 = (uint32_t)((inArg0 >= 0) ? inArg0 : (-inArg0));
	uint32_t absArg1 = (uint32_t)((inArg1 >= 0) ? inArg1 : (-inArg1));
	uint32_t A = (absArg0 >> 16), C = (absArg1 >> 16);
	uint32_t B = (absArg0 & 0xFFFF), D = (absArg1 & 0xFFFF);

	uint32_t AC = A * C;
	uint32_t AD_CB = A * D + C * B;
	uint32_t BD = B * D;

	uint32_t product_hi = AC + (AD_CB >> 16);

	uint32_t ad_cb_temp = AD_CB << 16;
	uint32_t product_lo = BD + ad_cb_temp;
	if (product_lo < BD)
		product_hi++;

	if (product_hi >> 15)
		return (fix16_t)FIX16_OVERFLOW;

	uint32_t product_lo_tmp = product_lo;
	product_lo += 0x8000;
	if (product_lo < product_lo_tmp)
		product_hi++;

	fix16_t result = (fix16_t)((product_hi << 16) | (product_lo >> 16));
	if ((inArg0 < 0) != (inArg1 < 0))
		result = -result;
	return result;
}

static inline fix16_t fix16_div(fix16_t a, fix16_t b)
{
	if (b == 0)
		return (fix16_t)FIX16_MINIMUM;

	uint32_t remainder = (uint32_t)((a >= 0) ? a : (-a));
	uint32_t divider = (uint32_t)((b >= 0) ? b : (-b));

	uint32_t quotient = 0;
	uint32_t bit = 0x10000;

	while (divider < remainder) {
		divider <<= 1;
		bit <<= 1;
	}

	if (!bit)
		return (fix16_t)FIX16_OVERFLOW;

	if (divider & 0x80000000) {
		if (remainder >= divider) {
			quotient |= bit;
			remainder -= divider;
		}
		divider >>= 1;
		bit >>= 1;
	}

	while (bit && remainder) {
		if (remainder >= divider) {
			quotient |= bit;
			remainder -= divider;
		}
		remainder <<= 1;
		bit >>= 1;
	}

	if (remainder >= divider)
		quotient++;

	fix16_t result = (fix16_t)quotient;

	if ((a < 0) != (b < 0)) {
		if (result == (fix16_t)FIX16_MINIMUM)
			return (fix16_t)FIX16_OVERFLOW;
		result = -result;
	}

	return result;
}

static inline fix16_t fix16_sqrt(fix16_t x)
{
	uint32_t num = (uint32_t)x;
	uint32_t result = 0;
	uint32_t bit;
	uint8_t n;

	bit = (uint32_t)1 << 30;
	while (bit > num)
		bit >>= 2;

	for (n = 0; n < 2; n++) {
		while (bit) {
			if (num >= result + bit) {
				num -= result + bit;
				result = (result >> 1) + bit;
			} else {
				result = (result >> 1);
			}
			bit >>= 2;
		}

		if (n == 0) {
			if (num > 65535) {
				num -= result;
				num = (num << 16) - 0x8000;
				result = (result << 16) + 0x8000;
			} else {
				num <<= 16;
				result <<= 16;
			}
			bit = 1 << 14;
		}
	}

	if (num > result)
		result++;

	return (fix16_t)result;
}

static inline fix16_t fix16_exp(fix16_t x)
{
#define SGP40_NUM_EXP_VALUES 4
	static const fix16_t exp_pos_values[SGP40_NUM_EXP_VALUES] = {
		F16(2.7182818), F16(1.1331485), F16(1.0157477), F16(1.0019550)
	};
	static const fix16_t exp_neg_values[SGP40_NUM_EXP_VALUES] = {
		F16(0.3678794), F16(0.8824969), F16(0.9844964), F16(0.9980488)
	};
	const fix16_t *exp_values;

	fix16_t res, arg;
	uint16_t i;

	if (x >= F16(10.3972))
		return FIX16_MAXIMUM;
	if (x <= F16(-11.7835))
		return 0;

	if (x < 0) {
		x = -x;
		exp_values = exp_neg_values;
	} else {
		exp_values = exp_pos_values;
	}

	res = FIX16_ONE;
	arg = FIX16_ONE;
	for (i = 0; i < SGP40_NUM_EXP_VALUES; i++) {
		while (x >= arg) {
			res = fix16_mul(res, exp_values[i]);
			x -= arg;
		}
		arg >>= 3;
	}
	return res;
#undef SGP40_NUM_EXP_VALUES
}

/*
 * Persistent state for the Sensirion VOC Index Algorithm.
 *
 * Field names and types are mirrored verbatim from GasIndexAlgorithmParams
 * in the upstream fixpoint reference so the port can be diffed cleanly
 * against it. NOX-related fields are dropped; this is VOC-only.
 */
struct sgp40_voc_algo_state {
	int32_t mAlgorithm_Type;
	fix16_t mIndex_Offset;
	int32_t mSraw_Minimum;
	fix16_t mGating_Max_Duration_Minutes;
	fix16_t mInit_Duration_Mean;
	fix16_t mInit_Duration_Variance;
	fix16_t mGating_Threshold;
	fix16_t mIndex_Gain;
	fix16_t mTau_Mean_Hours;
	fix16_t mTau_Variance_Hours;
	fix16_t mSraw_Std_Initial;
	fix16_t mUptime;
	fix16_t mSraw;
	fix16_t mGas_Index;
	bool m_Mean_Variance_Estimator___Initialized;
	fix16_t m_Mean_Variance_Estimator___Mean;
	fix16_t m_Mean_Variance_Estimator___Sraw_Offset;
	fix16_t m_Mean_Variance_Estimator___Std;
	fix16_t m_Mean_Variance_Estimator___Gamma_Mean;
	fix16_t m_Mean_Variance_Estimator___Gamma_Variance;
	fix16_t m_Mean_Variance_Estimator___Gamma_Initial_Mean;
	fix16_t m_Mean_Variance_Estimator___Gamma_Initial_Variance;
	fix16_t m_Mean_Variance_Estimator__Gamma_Mean;
	fix16_t m_Mean_Variance_Estimator__Gamma_Variance;
	fix16_t m_Mean_Variance_Estimator___Uptime_Gamma;
	fix16_t m_Mean_Variance_Estimator___Uptime_Gating;
	fix16_t m_Mean_Variance_Estimator___Gating_Duration_Minutes;
	fix16_t m_Mean_Variance_Estimator___Sigmoid__K;
	fix16_t m_Mean_Variance_Estimator___Sigmoid__X0;
	fix16_t m_Mox_Model__Sraw_Std;
	fix16_t m_Mox_Model__Sraw_Mean;
	fix16_t m_Sigmoid_Scaled__K;
	fix16_t m_Sigmoid_Scaled__X0;
	fix16_t m_Sigmoid_Scaled__Offset_Default;
	fix16_t m_Adaptive_Lowpass__A1;
	fix16_t m_Adaptive_Lowpass__A2;
	bool m_Adaptive_Lowpass___Initialized;
	fix16_t m_Adaptive_Lowpass___X1;
	fix16_t m_Adaptive_Lowpass___X2;
	fix16_t m_Adaptive_Lowpass___X3;
};

void sgp40_voc_algo_init(struct sgp40_voc_algo_state *algo);
int32_t sgp40_voc_algo_process(struct sgp40_voc_algo_state *algo,
			       uint16_t sraw);

#endif /* SGP40_VOC_ALGO_H_ */
