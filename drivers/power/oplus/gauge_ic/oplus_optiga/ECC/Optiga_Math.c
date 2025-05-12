/**
 * @file   Optiga_Math.c
 * @date   September, 2012
   @brief  ECC crypto engine
 *
 */

#include "Optiga_Math.h"
#include "Optiga_Ecc.h"

/** pointers to parameters of base point */
eccpoint_t actual_base_point[1];   /* coordinates of base point */
uint32_t *actual_base_point_order; /* order of the base point */

/** pointers to actual parameters of the elliptic curve */
uint32_t *actual_coeff_a;      /* curve parameter a */
uint32_t *actual_coeff_sqrt_b; /* square root of curve parameter b */

/** pointers to actual parameters of the finite field */
uint32_t *actual_irred_polynomial;
unsigned int actual_degree;

/** pointers to actual arithmetic functions for GF(2^n) */
static func2_pt actual_dwordvec_l_shift;

func2_pt actual_gf2n_sum, actual_gf2n_square, actual_dwordvec_copy;
func3_pt actual_gf2n_add, actual_gf2n_mul;

/** irreducible polynomials for different field sizes */
const uint32_t irred_polynomial_131[ARRAY_LEN(GF2_131)] =
{0x10d, 0x0, 0x0, 0x0, 0x8};
static const uint32_t irred_polynomial_163[ARRAY_LEN(GF2_163)] =
{0xc9, 0x0, 0x0, 0x0, 0x0, 0x8};

static const uint32_t K[64] = {
		0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
		0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
		0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
		0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
		0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
		0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
		0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
		0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

/** precomputed lookup table for faster squaring in GF(2^n) */
static const uint16_t square_tab[256] = 
{
		0x0, 0x1, 0x4, 0x5, 0x10, 0x11, 0x14, 0x15,
		0x40, 0x41, 0x44, 0x45, 0x50, 0x51, 0x54, 0x55,
		0x100, 0x101, 0x104, 0x105, 0x110, 0x111, 0x114, 0x115,
		0x140, 0x141, 0x144, 0x145, 0x150, 0x151, 0x154, 0x155,
		0x400, 0x401, 0x404, 0x405, 0x410, 0x411, 0x414, 0x415,
		0x440, 0x441, 0x444, 0x445, 0x450, 0x451, 0x454, 0x455,
		0x500, 0x501, 0x504, 0x505, 0x510, 0x511, 0x514, 0x515,
		0x540, 0x541, 0x544, 0x545, 0x550, 0x551, 0x554, 0x555,
		0x1000, 0x1001, 0x1004, 0x1005, 0x1010, 0x1011, 0x1014, 0x1015,
		0x1040, 0x1041, 0x1044, 0x1045, 0x1050, 0x1051, 0x1054, 0x1055,
		0x1100, 0x1101, 0x1104, 0x1105, 0x1110, 0x1111, 0x1114, 0x1115,
		0x1140, 0x1141, 0x1144, 0x1145, 0x1150, 0x1151, 0x1154, 0x1155,
		0x1400, 0x1401, 0x1404, 0x1405, 0x1410, 0x1411, 0x1414, 0x1415,
		0x1440, 0x1441, 0x1444, 0x1445, 0x1450, 0x1451, 0x1454, 0x1455,
		0x1500, 0x1501, 0x1504, 0x1505, 0x1510, 0x1511, 0x1514, 0x1515,
		0x1540, 0x1541, 0x1544, 0x1545, 0x1550, 0x1551, 0x1554, 0x1555,
		0x4000, 0x4001, 0x4004, 0x4005, 0x4010, 0x4011, 0x4014, 0x4015,
		0x4040, 0x4041, 0x4044, 0x4045, 0x4050, 0x4051, 0x4054, 0x4055,
		0x4100, 0x4101, 0x4104, 0x4105, 0x4110, 0x4111, 0x4114, 0x4115,
		0x4140, 0x4141, 0x4144, 0x4145, 0x4150, 0x4151, 0x4154, 0x4155,
		0x4400, 0x4401, 0x4404, 0x4405, 0x4410, 0x4411, 0x4414, 0x4415,
		0x4440, 0x4441, 0x4444, 0x4445, 0x4450, 0x4451, 0x4454, 0x4455,
		0x4500, 0x4501, 0x4504, 0x4505, 0x4510, 0x4511, 0x4514, 0x4515,
		0x4540, 0x4541, 0x4544, 0x4545, 0x4550, 0x4551, 0x4554, 0x4555,
		0x5000, 0x5001, 0x5004, 0x5005, 0x5010, 0x5011, 0x5014, 0x5015,
		0x5040, 0x5041, 0x5044, 0x5045, 0x5050, 0x5051, 0x5054, 0x5055,
		0x5100, 0x5101, 0x5104, 0x5105, 0x5110, 0x5111, 0x5114, 0x5115,
		0x5140, 0x5141, 0x5144, 0x5145, 0x5150, 0x5151, 0x5154, 0x5155,
		0x5400, 0x5401, 0x5404, 0x5405, 0x5410, 0x5411, 0x5414, 0x5415,
		0x5440, 0x5441, 0x5444, 0x5445, 0x5450, 0x5451, 0x5454, 0x5455,
		0x5500, 0x5501, 0x5504, 0x5505, 0x5510, 0x5511, 0x5514, 0x5515,
		0x5540, 0x5541, 0x5544, 0x5545, 0x5550, 0x5551, 0x5554, 0x5555
};

/** constant field element with value 1*/
static const dwordvec_t one_element = {0x1, 0x0, 0x0, 0x0, 0x0, 0x0};

/** constant field element with value 0*/
static const dwordvec_t zero_element = {0x0, 0x0, 0x0, 0x0, 0x0, 0x0};

/* ------------------------------------------------------------------------- */
/** challenge generation
 * Description:  This function creates the challenge.
 * 
 * [out] xA challenge, which is the affine x-coordinate
 *       of A =  lambda *  P
 * [in] curve parameters of the elliptic curve
 * return FALSE if everything is ok, TRUE otherwise
 ********************************************************************/
BOOL generate_challenge (dwordvec_t xA, const curve_parameter_t *curve, dwordvec_t lambda)
{
	dwordvec_t A, B, C, D;

	ecc_init(curve);

	mont_ecc_mul(A, B, C, D, actual_base_point->x_coord, lambda);

	return gf2n_divide(xA, A, B);
}


/********************************************************************
 * Function Name: generate_checkvalue
 * Description:  This function generate check value
 * 
 * [out] xC check value for the response, which is the affine x-coordinate
 *             of \a C = \a lambda * \a xT
 * [in] xT public key, which is the affine x-coordinate of \a T = \a xiT * \a P
 * [in] curve parameters of the elliptic curve
 * return FALSE if everything is ok, TRUE otherwise
 ********************************************************************/
BOOL generate_checkvalue (dwordvec_t xC, const dwordvec_t xT, const curve_parameter_t *curve, dwordvec_t lambda)
{
	dwordvec_t A, B, C, D;

	ecc_init(curve);

	mont_ecc_mul(A, B, C, D, xT, lambda);

	return gf2n_divide(xC, A, B);
}

/********************************************************************
 * Function Name: verify_response
 * Description:  This function verifies the response from the IC
 *
 * [in] gf2n_XResponse X response
 * [in] gf2n_ZResponse Z response
 * [in] gf2n_CheckValue check value for the response
 * [in] curve parameters of the elliptic curve
 * return FALSE if everything is ok, TRUE otherwise
 ********************************************************************/
BOOL verify_response( dwordvec_t gf2n_XResponse, dwordvec_t gf2n_ZResponse, dwordvec_t gf2n_CheckValue, const curve_parameter_t *curve )
{
	uint32_t uwResult;
	dwordvec_t gf2n_AV;

	ecc_init(curve);

	/* z must not be zero */
	if( dwordvec_iszero(gf2n_ZResponse) == TRUE )
	{
		return FALSE;
	}
	/* do last calculation step and check values */
	gf2n_mul( gf2n_AV, gf2n_CheckValue, gf2n_ZResponse);
	uwResult  = (gf2n_XResponse[0] ^ gf2n_AV[0]);
	uwResult |= (gf2n_XResponse[1] ^ gf2n_AV[1]);
	//uwResult |= (gf2n_XResponse[2] ^ gf2n_AV[2]);
	//uwResult |= (gf2n_XResponse[3] ^ gf2n_AV[3]);

	/* check state of authentication */
	if( uwResult == 0 )
	{
		return TRUE;
	}

	/* authentication failed */
	return FALSE;
}

#if defined (SLE95150) || defined (SLE95250) || defined (SLE95300)
/* ------------------------------------------------------------------------- */
/** verify MAC value
 * param[in] mac_value MAC computed by the tag
 * param[in] Z projective z-coordinate of the response of the tag
 * param[in] xC expected response (i.e. check value), which is the affine
 *            x-coordinate of \a C = \a lambda * \a xT
 * param[in] data data to be auhenticated (i.e. counter value)
 * param[in] curve parameters of the elliptic curve
 * return TRUE if mac/response is valid, FALSE otherwise
 */
BOOL verify_mac64(const mac_t mac_value, const dwordvec_t Z, const dwordvec_t xC, const mac_t data, const curve_parameter_t *curve, mac_t host_mac_value)
{
	dwordvec_t host_session_key;
	//  mac_t host_mac_value;
	uint32_t t;

	/* initialize finite field */
	gf2n_init(curve->degree);

	/* verify MAC */
	if (dwordvec_iszero(Z))
	{
		return FALSE;
	}

	gf2n_mul(host_session_key, xC, Z);

	mac_algorithm_64(host_mac_value, data, host_session_key);

	t = host_mac_value[0] ^ mac_value[0];
	t |= host_mac_value[1] ^ mac_value[1];

	if (actual_degree == GF2_163)
	{
		t |= host_mac_value[2] ^ mac_value[2];
	}

	if (t)
	{
		return FALSE;
	}

	return TRUE;
}
#endif

/* ------------------------------------------------------------------------- */