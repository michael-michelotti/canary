// SPDX-License-Identifier: GPL-2.0+
/*
 * sgp40_voc_algo.c - Kernel-space port of Sensirion's VOC Index Algorithm.
 *
 * Line-for-line port of the fixpoint reference at
 *   https://github.com/Sensirion/gas-index-algorithm
 *   sensirion_gas_index_algorithm_fixpoint/sensirion_gas_index_algorithm.c
 * with the NOX paths stripped (SGP40 is VOC-only hardware).
 *
 * Upstream is Copyright (c) 2022, Sensirion AG, BSD-3-Clause. This port is
 * released under GPL-2.0+ for in-kernel use; the BSD-3-Clause terms of the
 * original continue to apply to anyone lifting the algorithm out of here.
 *
 * Constant names, struct field names, and function decomposition are kept
 * identical to the reference so the two can be diffed sample-by-sample for
 * numerical validation. Each function below names the reference function
 * it ports. Fixed-point primitives live in sgp40_voc_algo.h and are
 * likewise taken verbatim from Sensirion's library.
 */

#include <linux/string.h>
#include <linux/types.h>

#include "sgp40_voc_algo.h"

#define SGP40_VOC_SAMPLING_INTERVAL			(1.)
#define SGP40_VOC_INDEX_GAIN				(230.)
#define SGP40_VOC_SRAW_STD_INITIAL			(50.)
#define SGP40_VOC_SRAW_STD_BONUS_VOC			(220.)
#define SGP40_VOC_TAU_MEAN_HOURS			(12.)
#define SGP40_VOC_TAU_VARIANCE_HOURS			(12.)
#define SGP40_VOC_TAU_INITIAL_MEAN_VOC			(20.)
#define SGP40_VOC_INIT_DURATION_MEAN_VOC		((3600. * 0.75))
#define SGP40_VOC_INIT_TRANSITION_MEAN			(0.01)
#define SGP40_VOC_TAU_INITIAL_VARIANCE			(2500.)
#define SGP40_VOC_INIT_DURATION_VARIANCE_VOC		((3600. * 1.45))
#define SGP40_VOC_INIT_TRANSITION_VARIANCE		(0.01)
#define SGP40_VOC_GATING_THRESHOLD_VOC			(340.)
#define SGP40_VOC_GATING_THRESHOLD_INITIAL		(510.)
#define SGP40_VOC_GATING_THRESHOLD_TRANSITION		(0.09)
#define SGP40_VOC_GATING_VOC_MAX_DURATION_MINUTES	((60. * 3.))
#define SGP40_VOC_GATING_MAX_RATIO			(0.3)
#define SGP40_VOC_MVE_FIX16_MAX				(32767.)
#define SGP40_VOC_SIGMOID_L				(500.)
#define SGP40_VOC_SIGMOID_X0_VOC			(213.)
#define SGP40_VOC_SIGMOID_K_VOC				(-0.0065)
#define SGP40_VOC_VOC_INDEX_OFFSET_DEFAULT		(100.)
#define SGP40_VOC_LP_TAU_FAST				(20.0)
#define SGP40_VOC_LP_TAU_SLOW				(500.0)
#define SGP40_VOC_LP_ALPHA				(-0.2)
#define SGP40_VOC_VOC_SRAW_MINIMUM			(20000)
#define SGP40_VOC_INITIAL_BLACKOUT			(45.)
#define SGP40_VOC_MVE_GAMMA_SCALING			(64.)
#define SGP40_VOC_MVE_ADDITIONAL_GAMMA_MEAN_SCALING	(8.)

/* Ref: GasIndexAlgorithm__mean_variance_estimator__set_parameters */
static void sgp40_voc_mve_set_parameters(struct sgp40_voc_algo_state *algo)
{
	algo->m_Mean_Variance_Estimator___Initialized = false;
	algo->m_Mean_Variance_Estimator___Mean = F16(0.);
	algo->m_Mean_Variance_Estimator___Sraw_Offset = F16(0.);
	algo->m_Mean_Variance_Estimator___Std = algo->mSraw_Std_Initial;

	algo->m_Mean_Variance_Estimator___Gamma_Mean = fix16_div(
		F16((SGP40_VOC_MVE_ADDITIONAL_GAMMA_MEAN_SCALING *
		     SGP40_VOC_MVE_GAMMA_SCALING) *
		    (SGP40_VOC_SAMPLING_INTERVAL / 3600.)),
		algo->mTau_Mean_Hours +
			F16(SGP40_VOC_SAMPLING_INTERVAL / 3600.));

	algo->m_Mean_Variance_Estimator___Gamma_Variance = fix16_div(
		F16(SGP40_VOC_MVE_GAMMA_SCALING *
		    (SGP40_VOC_SAMPLING_INTERVAL / 3600.)),
		algo->mTau_Variance_Hours +
			F16(SGP40_VOC_SAMPLING_INTERVAL / 3600.));

	algo->m_Mean_Variance_Estimator___Gamma_Initial_Mean = F16(
		((SGP40_VOC_MVE_ADDITIONAL_GAMMA_MEAN_SCALING *
		  SGP40_VOC_MVE_GAMMA_SCALING) *
		 SGP40_VOC_SAMPLING_INTERVAL) /
		(SGP40_VOC_TAU_INITIAL_MEAN_VOC + SGP40_VOC_SAMPLING_INTERVAL));

	algo->m_Mean_Variance_Estimator___Gamma_Initial_Variance = F16(
		(SGP40_VOC_MVE_GAMMA_SCALING * SGP40_VOC_SAMPLING_INTERVAL) /
		(SGP40_VOC_TAU_INITIAL_VARIANCE + SGP40_VOC_SAMPLING_INTERVAL));

	algo->m_Mean_Variance_Estimator__Gamma_Mean = F16(0.);
	algo->m_Mean_Variance_Estimator__Gamma_Variance = F16(0.);
	algo->m_Mean_Variance_Estimator___Uptime_Gamma = F16(0.);
	algo->m_Mean_Variance_Estimator___Uptime_Gating = F16(0.);
	algo->m_Mean_Variance_Estimator___Gating_Duration_Minutes = F16(0.);
}

/* Ref: GasIndexAlgorithm__mean_variance_estimator__get_std */
static fix16_t sgp40_voc_mve_get_std(const struct sgp40_voc_algo_state *algo)
{
	return algo->m_Mean_Variance_Estimator___Std;
}

/* Ref: GasIndexAlgorithm__mean_variance_estimator__get_mean */
static fix16_t sgp40_voc_mve_get_mean(const struct sgp40_voc_algo_state *algo)
{
	return algo->m_Mean_Variance_Estimator___Mean +
	       algo->m_Mean_Variance_Estimator___Sraw_Offset;
}

/* Ref: GasIndexAlgorithm__mean_variance_estimator___sigmoid__set_parameters */
static void sgp40_voc_mve_sigmoid_set_parameters(
	struct sgp40_voc_algo_state *algo, fix16_t x0, fix16_t k)
{
	algo->m_Mean_Variance_Estimator___Sigmoid__K = k;
	algo->m_Mean_Variance_Estimator___Sigmoid__X0 = x0;
}

/* Ref: GasIndexAlgorithm__mean_variance_estimator___sigmoid__process */
static fix16_t sgp40_voc_mve_sigmoid_process(
	struct sgp40_voc_algo_state *algo, fix16_t sample)
{
	fix16_t x;

	x = fix16_mul(algo->m_Mean_Variance_Estimator___Sigmoid__K,
		      sample - algo->m_Mean_Variance_Estimator___Sigmoid__X0);

	if (x < F16(-50.))
		return F16(1.);
	if (x > F16(50.))
		return F16(0.);
	return fix16_div(F16(1.), F16(1.) + fix16_exp(x));
}

/* Ref: GasIndexAlgorithm__mean_variance_estimator___calculate_gamma */
static void sgp40_voc_mve_calculate_gamma(struct sgp40_voc_algo_state *algo)
{
	fix16_t uptime_limit;
	fix16_t sigmoid_gamma_mean;
	fix16_t gamma_mean;
	fix16_t gating_threshold_mean;
	fix16_t sigmoid_gating_mean;
	fix16_t sigmoid_gamma_variance;
	fix16_t gamma_variance;
	fix16_t gating_threshold_variance;
	fix16_t sigmoid_gating_variance;

	uptime_limit = F16(SGP40_VOC_MVE_FIX16_MAX - SGP40_VOC_SAMPLING_INTERVAL);
	if (algo->m_Mean_Variance_Estimator___Uptime_Gamma < uptime_limit)
		algo->m_Mean_Variance_Estimator___Uptime_Gamma +=
			F16(SGP40_VOC_SAMPLING_INTERVAL);
	if (algo->m_Mean_Variance_Estimator___Uptime_Gating < uptime_limit)
		algo->m_Mean_Variance_Estimator___Uptime_Gating +=
			F16(SGP40_VOC_SAMPLING_INTERVAL);

	sgp40_voc_mve_sigmoid_set_parameters(
		algo, algo->mInit_Duration_Mean,
		F16(SGP40_VOC_INIT_TRANSITION_MEAN));
	sigmoid_gamma_mean = sgp40_voc_mve_sigmoid_process(
		algo, algo->m_Mean_Variance_Estimator___Uptime_Gamma);
	gamma_mean = algo->m_Mean_Variance_Estimator___Gamma_Mean +
		     fix16_mul(
			     algo->m_Mean_Variance_Estimator___Gamma_Initial_Mean -
				     algo->m_Mean_Variance_Estimator___Gamma_Mean,
			     sigmoid_gamma_mean);

	gating_threshold_mean =
		algo->mGating_Threshold +
		fix16_mul(
			F16(SGP40_VOC_GATING_THRESHOLD_INITIAL) - algo->mGating_Threshold,
			sgp40_voc_mve_sigmoid_process(
				algo, algo->m_Mean_Variance_Estimator___Uptime_Gating));
	sgp40_voc_mve_sigmoid_set_parameters(
		algo, gating_threshold_mean,
		F16(SGP40_VOC_GATING_THRESHOLD_TRANSITION));
	sigmoid_gating_mean =
		sgp40_voc_mve_sigmoid_process(algo, algo->mGas_Index);
	algo->m_Mean_Variance_Estimator__Gamma_Mean =
		fix16_mul(sigmoid_gating_mean, gamma_mean);

	sgp40_voc_mve_sigmoid_set_parameters(
		algo, algo->mInit_Duration_Variance,
		F16(SGP40_VOC_INIT_TRANSITION_VARIANCE));
	sigmoid_gamma_variance = sgp40_voc_mve_sigmoid_process(
		algo, algo->m_Mean_Variance_Estimator___Uptime_Gamma);
	gamma_variance =
		algo->m_Mean_Variance_Estimator___Gamma_Variance +
		fix16_mul(
			algo->m_Mean_Variance_Estimator___Gamma_Initial_Variance -
				algo->m_Mean_Variance_Estimator___Gamma_Variance,
			sigmoid_gamma_variance - sigmoid_gamma_mean);

	gating_threshold_variance =
		algo->mGating_Threshold +
		fix16_mul(
			F16(SGP40_VOC_GATING_THRESHOLD_INITIAL) - algo->mGating_Threshold,
			sgp40_voc_mve_sigmoid_process(
				algo, algo->m_Mean_Variance_Estimator___Uptime_Gating));
	sgp40_voc_mve_sigmoid_set_parameters(
		algo, gating_threshold_variance,
		F16(SGP40_VOC_GATING_THRESHOLD_TRANSITION));
	sigmoid_gating_variance =
		sgp40_voc_mve_sigmoid_process(algo, algo->mGas_Index);
	algo->m_Mean_Variance_Estimator__Gamma_Variance =
		fix16_mul(sigmoid_gating_variance, gamma_variance);

	algo->m_Mean_Variance_Estimator___Gating_Duration_Minutes +=
		fix16_mul(
			F16(SGP40_VOC_SAMPLING_INTERVAL / 60.),
			fix16_mul(F16(1.) - sigmoid_gating_mean,
				  F16(1. + SGP40_VOC_GATING_MAX_RATIO)) -
				F16(SGP40_VOC_GATING_MAX_RATIO));

	if (algo->m_Mean_Variance_Estimator___Gating_Duration_Minutes < F16(0.))
		algo->m_Mean_Variance_Estimator___Gating_Duration_Minutes = F16(0.);
	if (algo->m_Mean_Variance_Estimator___Gating_Duration_Minutes >
	    algo->mGating_Max_Duration_Minutes)
		algo->m_Mean_Variance_Estimator___Uptime_Gating = F16(0.);
}

/* Ref: GasIndexAlgorithm__mean_variance_estimator__process */
static void sgp40_voc_mve_process(struct sgp40_voc_algo_state *algo,
				  fix16_t sraw)
{
	fix16_t delta_sgp;
	fix16_t c;
	fix16_t additional_scaling;

	if (!algo->m_Mean_Variance_Estimator___Initialized) {
		algo->m_Mean_Variance_Estimator___Initialized = true;
		algo->m_Mean_Variance_Estimator___Sraw_Offset = sraw;
		algo->m_Mean_Variance_Estimator___Mean = F16(0.);
		return;
	}

	if (algo->m_Mean_Variance_Estimator___Mean >= F16(100.) ||
	    algo->m_Mean_Variance_Estimator___Mean <= F16(-100.)) {
		algo->m_Mean_Variance_Estimator___Sraw_Offset +=
			algo->m_Mean_Variance_Estimator___Mean;
		algo->m_Mean_Variance_Estimator___Mean = F16(0.);
	}
	sraw -= algo->m_Mean_Variance_Estimator___Sraw_Offset;

	sgp40_voc_mve_calculate_gamma(algo);

	delta_sgp = fix16_div(sraw - algo->m_Mean_Variance_Estimator___Mean,
			      F16(SGP40_VOC_MVE_GAMMA_SCALING));

	if (delta_sgp < F16(0.))
		c = algo->m_Mean_Variance_Estimator___Std - delta_sgp;
	else
		c = algo->m_Mean_Variance_Estimator___Std + delta_sgp;

	additional_scaling = F16(1.);
	if (c > F16(1440.))
		additional_scaling = fix16_mul(fix16_div(c, F16(1440.)),
					       fix16_div(c, F16(1440.)));

	algo->m_Mean_Variance_Estimator___Std = fix16_mul(
		fix16_sqrt(fix16_mul(
			additional_scaling,
			F16(SGP40_VOC_MVE_GAMMA_SCALING) -
				algo->m_Mean_Variance_Estimator__Gamma_Variance)),
		fix16_sqrt(
			fix16_mul(algo->m_Mean_Variance_Estimator___Std,
				  fix16_div(algo->m_Mean_Variance_Estimator___Std,
					    fix16_mul(F16(SGP40_VOC_MVE_GAMMA_SCALING),
						      additional_scaling))) +
			fix16_mul(
				fix16_div(
					fix16_mul(algo->m_Mean_Variance_Estimator__Gamma_Variance,
						  delta_sgp),
					additional_scaling),
				delta_sgp)));

	algo->m_Mean_Variance_Estimator___Mean +=
		fix16_div(
			fix16_mul(algo->m_Mean_Variance_Estimator__Gamma_Mean, delta_sgp),
			F16(SGP40_VOC_MVE_ADDITIONAL_GAMMA_MEAN_SCALING));
}

/* Ref: GasIndexAlgorithm__mox_model__set_parameters */
static void sgp40_voc_mox_set_parameters(struct sgp40_voc_algo_state *algo,
					 fix16_t sraw_std, fix16_t sraw_mean)
{
	algo->m_Mox_Model__Sraw_Std = sraw_std;
	algo->m_Mox_Model__Sraw_Mean = sraw_mean;
}

/* Ref: GasIndexAlgorithm__mox_model__process */
static fix16_t sgp40_voc_mox_process(struct sgp40_voc_algo_state *algo,
				     fix16_t sraw)
{
	return fix16_mul(
		fix16_div(sraw - algo->m_Mox_Model__Sraw_Mean,
			  -(algo->m_Mox_Model__Sraw_Std +
			    F16(SGP40_VOC_SRAW_STD_BONUS_VOC))),
		algo->mIndex_Gain);
}

/* Ref: GasIndexAlgorithm__sigmoid_scaled__set_parameters */
static void sgp40_voc_sigmoid_scaled_set_parameters(
	struct sgp40_voc_algo_state *algo,
	fix16_t x0, fix16_t k, fix16_t offset_default)
{
	algo->m_Sigmoid_Scaled__K = k;
	algo->m_Sigmoid_Scaled__X0 = x0;
	algo->m_Sigmoid_Scaled__Offset_Default = offset_default;
}

/* Ref: GasIndexAlgorithm__sigmoid_scaled__process */
static fix16_t sgp40_voc_sigmoid_scaled_process(
	struct sgp40_voc_algo_state *algo, fix16_t sample)
{
	fix16_t x;
	fix16_t shift;

	x = fix16_mul(algo->m_Sigmoid_Scaled__K,
		      sample - algo->m_Sigmoid_Scaled__X0);

	if (x < F16(-50.))
		return F16(SGP40_VOC_SIGMOID_L);
	if (x > F16(50.))
		return F16(0.);

	if (sample >= F16(0.)) {
		shift = fix16_div(
			F16(SGP40_VOC_SIGMOID_L) -
				fix16_mul(F16(5.), algo->mIndex_Offset),
			F16(4.));
		return fix16_div(F16(SGP40_VOC_SIGMOID_L) + shift,
				 F16(1.) + fix16_exp(x)) -
		       shift;
	}

	return fix16_mul(
		fix16_div(algo->mIndex_Offset,
			  algo->m_Sigmoid_Scaled__Offset_Default),
		fix16_div(F16(SGP40_VOC_SIGMOID_L),
			  F16(1.) + fix16_exp(x)));
}

/* Ref: GasIndexAlgorithm__adaptive_lowpass__set_parameters */
static void sgp40_voc_lowpass_set_parameters(struct sgp40_voc_algo_state *algo)
{
	algo->m_Adaptive_Lowpass__A1 = F16(
		SGP40_VOC_SAMPLING_INTERVAL /
		(SGP40_VOC_LP_TAU_FAST + SGP40_VOC_SAMPLING_INTERVAL));
	algo->m_Adaptive_Lowpass__A2 = F16(
		SGP40_VOC_SAMPLING_INTERVAL /
		(SGP40_VOC_LP_TAU_SLOW + SGP40_VOC_SAMPLING_INTERVAL));
	algo->m_Adaptive_Lowpass___Initialized = false;
}

/* Ref: GasIndexAlgorithm__adaptive_lowpass__process */
static fix16_t sgp40_voc_lowpass_process(struct sgp40_voc_algo_state *algo,
					 fix16_t sample)
{
	fix16_t abs_delta;
	fix16_t F1;
	fix16_t tau_a;
	fix16_t a3;

	if (!algo->m_Adaptive_Lowpass___Initialized) {
		algo->m_Adaptive_Lowpass___X1 = sample;
		algo->m_Adaptive_Lowpass___X2 = sample;
		algo->m_Adaptive_Lowpass___X3 = sample;
		algo->m_Adaptive_Lowpass___Initialized = true;
	}

	algo->m_Adaptive_Lowpass___X1 =
		fix16_mul(F16(1.) - algo->m_Adaptive_Lowpass__A1,
			  algo->m_Adaptive_Lowpass___X1) +
		fix16_mul(algo->m_Adaptive_Lowpass__A1, sample);

	algo->m_Adaptive_Lowpass___X2 =
		fix16_mul(F16(1.) - algo->m_Adaptive_Lowpass__A2,
			  algo->m_Adaptive_Lowpass___X2) +
		fix16_mul(algo->m_Adaptive_Lowpass__A2, sample);

	abs_delta = algo->m_Adaptive_Lowpass___X1 -
		    algo->m_Adaptive_Lowpass___X2;
	if (abs_delta < F16(0.))
		abs_delta = -abs_delta;

	F1 = fix16_exp(fix16_mul(F16(SGP40_VOC_LP_ALPHA), abs_delta));
	tau_a = fix16_mul(
			F16(SGP40_VOC_LP_TAU_SLOW - SGP40_VOC_LP_TAU_FAST), F1) +
		F16(SGP40_VOC_LP_TAU_FAST);
	a3 = fix16_div(F16(SGP40_VOC_SAMPLING_INTERVAL),
		       F16(SGP40_VOC_SAMPLING_INTERVAL) + tau_a);

	algo->m_Adaptive_Lowpass___X3 =
		fix16_mul(F16(1.) - a3, algo->m_Adaptive_Lowpass___X3) +
		fix16_mul(a3, sample);

	return algo->m_Adaptive_Lowpass___X3;
}

/* Ref: GasIndexAlgorithm_init + _reset + _init_instances (VOC paths only). */
void sgp40_voc_algo_init(struct sgp40_voc_algo_state *algo)
{
	memset(algo, 0, sizeof(*algo));

	algo->mAlgorithm_Type = 0;
	algo->mIndex_Offset = F16(SGP40_VOC_VOC_INDEX_OFFSET_DEFAULT);
	algo->mSraw_Minimum = SGP40_VOC_VOC_SRAW_MINIMUM;
	algo->mGating_Max_Duration_Minutes =
		F16(SGP40_VOC_GATING_VOC_MAX_DURATION_MINUTES);
	algo->mInit_Duration_Mean = F16(SGP40_VOC_INIT_DURATION_MEAN_VOC);
	algo->mInit_Duration_Variance = F16(SGP40_VOC_INIT_DURATION_VARIANCE_VOC);
	algo->mGating_Threshold = F16(SGP40_VOC_GATING_THRESHOLD_VOC);
	algo->mIndex_Gain = F16(SGP40_VOC_INDEX_GAIN);
	algo->mTau_Mean_Hours = F16(SGP40_VOC_TAU_MEAN_HOURS);
	algo->mTau_Variance_Hours = F16(SGP40_VOC_TAU_VARIANCE_HOURS);
	algo->mSraw_Std_Initial = F16(SGP40_VOC_SRAW_STD_INITIAL);

	algo->mUptime = F16(0.);
	algo->mSraw = F16(0.);
	algo->mGas_Index = 0;

	sgp40_voc_mve_set_parameters(algo);
	sgp40_voc_mox_set_parameters(algo,
				     sgp40_voc_mve_get_std(algo),
				     sgp40_voc_mve_get_mean(algo));
	sgp40_voc_sigmoid_scaled_set_parameters(
		algo,
		F16(SGP40_VOC_SIGMOID_X0_VOC),
		F16(SGP40_VOC_SIGMOID_K_VOC),
		F16(SGP40_VOC_VOC_INDEX_OFFSET_DEFAULT));
	sgp40_voc_lowpass_set_parameters(algo);
}

/* Ref: GasIndexAlgorithm_process. Must be called at 1 Hz. */
int32_t sgp40_voc_algo_process(struct sgp40_voc_algo_state *algo,
			       uint16_t sraw_in)
{
	int32_t sraw = sraw_in;

	if (algo->mUptime <= F16(SGP40_VOC_INITIAL_BLACKOUT)) {
		algo->mUptime += F16(SGP40_VOC_SAMPLING_INTERVAL);
	} else {
		if (sraw > 0 && sraw < 65000) {
			if (sraw < algo->mSraw_Minimum + 1)
				sraw = algo->mSraw_Minimum + 1;
			else if (sraw > algo->mSraw_Minimum + 32767)
				sraw = algo->mSraw_Minimum + 32767;
			algo->mSraw = fix16_from_int(sraw - algo->mSraw_Minimum);
		}

		algo->mGas_Index = sgp40_voc_mox_process(algo, algo->mSraw);
		algo->mGas_Index = sgp40_voc_sigmoid_scaled_process(algo,
								    algo->mGas_Index);

		algo->mGas_Index = sgp40_voc_lowpass_process(algo, algo->mGas_Index);
		if (algo->mGas_Index < F16(0.5))
			algo->mGas_Index = F16(0.5);

		if (algo->mSraw > F16(0.)) {
			sgp40_voc_mve_process(algo, algo->mSraw);
			sgp40_voc_mox_set_parameters(algo,
						     sgp40_voc_mve_get_std(algo),
						     sgp40_voc_mve_get_mean(algo));
		}
	}

	return fix16_cast_to_int(algo->mGas_Index + F16(0.5));
}
