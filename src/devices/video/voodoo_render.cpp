// license:BSD-3-Clause
// copyright-holders:Aaron Giles
/***************************************************************************

    voodoo_render.cpp

    3dfx Voodoo Graphics SST-1/2 emulator.

***************************************************************************/

#include "emu.h"
#include "voodoo.h"

namespace voodoo
{

/* fast dither lookup */
std::unique_ptr<u8[]> voodoo::dither_helper::s_dither4_lookup;
std::unique_ptr<u8[]> voodoo::dither_helper::s_dither2_lookup;
std::unique_ptr<u8[]> voodoo::dither_helper::s_nodither_lookup;

u8 const voodoo::dither_helper::s_dither_matrix_4x4[16] =
{
	 0,  8,  2, 10,
	12,  4, 14,  6,
	 3, 11,  1,  9,
	15,  7, 13,  5
};
// Using this matrix allows iteagle video memory tests to pass
u8 const voodoo::dither_helper::s_dither_matrix_2x2[16] =
{
	 8, 10,  8, 10,
	11,  9, 11,  9,
	 8, 10,  8, 10,
	11,  9, 11,  9
};

u8 const voodoo::dither_helper::s_dither_matrix_4x4_subtract[16] =
{
	(15 -  0) >> 1, (15 -  8) >> 1, (15 -  2) >> 1, (15 - 10) >> 1,
	(15 - 12) >> 1, (15 -  4) >> 1, (15 - 14) >> 1, (15 -  6) >> 1,
	(15 -  3) >> 1, (15 - 11) >> 1, (15 -  1) >> 1, (15 -  9) >> 1,
	(15 - 15) >> 1, (15 -  7) >> 1, (15 - 13) >> 1, (15 -  5) >> 1
};
u8 const voodoo::dither_helper::s_dither_matrix_2x2_subtract[16] =
{
	(15 -  8) >> 1, (15 - 10) >> 1, (15 -  8) >> 1, (15 - 10) >> 1,
	(15 - 11) >> 1, (15 -  9) >> 1, (15 - 11) >> 1, (15 -  9) >> 1,
	(15 -  8) >> 1, (15 - 10) >> 1, (15 -  8) >> 1, (15 - 10) >> 1,
	(15 - 11) >> 1, (15 -  9) >> 1, (15 - 11) >> 1, (15 -  9) >> 1
};



//**************************************************************************
//  STW HELPER
//**************************************************************************

// use SSE on 64-bit implementations, where it can be assumed
#if 1 && ((!defined(MAME_DEBUG) || defined(__OPTIMIZE__)) && (defined(__SSE2__) || defined(_MSC_VER)) && defined(PTR64))

#include <emmintrin.h>
#ifdef __SSE4_1__
#include <smmintrin.h>
#endif

class stw_helper
{
public:
	stw_helper() { }
	stw_helper(stw_helper const &other) = default;
	stw_helper &operator=(stw_helper const &other) = default;

	void set(s64 s, s64 t, s64 w)
	{
		m_st = _mm_set_pd(s << 8, t << 8);
		m_w = _mm_set1_pd(w);
	}

	bool is_w_neg() const
	{
		return _mm_comilt_sd(m_w, _mm_set1_pd(0.0));
	}

	void get_st_shiftr(s32 &s, s32 &t, s32 shift) const
	{
		shift += 8;
		s = _mm_cvtsd_si64(_mm_shuffle_pd(m_st, _mm_setzero_pd(), 1)) >> shift;
		t = _mm_cvtsd_si64(m_st) >> shift;
	}

	void add(stw_helper const &delta)
	{
		m_st = _mm_add_pd(m_st, delta.m_st);
		m_w = _mm_add_pd(m_w, delta.m_w);
	}

	void calc_stow(s32 &sow, s32 &tow, s32 &oowlog) const
	{
		__m128d tmp = _mm_div_pd(m_st, m_w);
		__m128i tmp2 = _mm_cvttpd_epi32(tmp);
#ifdef __SSE4_1__
		sow = _mm_extract_epi32(tmp2, 1);
		tow = _mm_extract_epi32(tmp2, 0);
#else
		sow = _mm_cvtsi128_si32(_mm_shuffle_epi32(tmp2, _MM_SHUFFLE(0, 0, 0, 1)));
		tow = _mm_cvtsi128_si32(tmp2);
#endif
		oowlog = -fast_log2(_mm_cvtsd_f64(m_w), 0);
	}

private:
	__m128d m_st;
	__m128d m_w;
};

#else

class stw_helper
{
public:
	stw_helper() {}
	stw_helper(stw_helper const &other) = default;
	stw_helper &operator=(stw_helper const &other) = default;

	void set(s64 s, s64 t, s64 w)
	{
		m_s = s;
		m_t = t;
		m_w = w;
	}

	bool is_w_neg() const
	{
		return (m_w < 0) ? true : false;
	}

	void get_st_shiftr(s32 &s, s32 &t, s32 shift) const
	{
		s = m_s >> shift;
		t = m_t >> shift;
	}

	inline void add(stw_helper const &other)
	{
		m_s += other.m_s;
		m_t += other.m_t;
		m_w += other.m_w;
	}

	// Computes s/w and t/w and returns log2 of 1/w
	// s, t and c are 16.32 values.  The results are 24.8.
	inline void calc_stow(s32 &sow, s32 &tow, s32 &oowlog) const
	{
		double recip = double(1ULL << (47 - 39)) / m_w;
		sow = s32(m_s * recip);
		tow = tow(m_t * recip);
		oowlog = fast_log2(recip, 56);
	}

private:
	s64 m_s, m_t, m_w;
};

#endif



/*************************************
 *
 *  Clamping macros
 *
 *************************************/

static inline rgbaint_t ATTR_FORCE_INLINE clamped_argb(const rgbaint_t &iterargb, reg_fbz_colorpath const fbzcp)
{
	rgbaint_t result(iterargb);
	result.shr_imm(20);

	// clamped case is easy
	if (fbzcp.rgbzw_clamp() != 0)
	{
		result.clamp_to_uint8();
		return result;
	}

	// for each component:
	//    result = val & 0xfff;
	//    if (result == 0xfff) result = 0;
	//    if (result == 0x100) result = 0xff;

	// check against 0xfff and force to 0
	rgbaint_t temp1(result);
	rgbaint_t temp2(result);
	temp1.cmpeq_imm(0xfff);
	temp2.cmpeq_imm(0x100);
	result.andnot_reg(temp1);
	result.or_reg(temp2);
	result.and_imm(0xff);
	return result;
}

static inline s32 ATTR_FORCE_INLINE clamped_z(s32 iterz, reg_fbz_colorpath const fbzcp)
{
	// clamped case is easy
	if (fbzcp.rgbzw_clamp() != 0)
		return std::clamp(iterz >> 12, 0, 0xffff);

	// non-clamped case has specific behaviors
	u32 result = u32(iterz) >> 12;
	if (result == 0xfffff)
		return 0;
	if (result == 0x10000)
		return 0xffff;
	return result & 0xffff;
}

static inline s32 ATTR_FORCE_INLINE clamped_w(s64 iterw, reg_fbz_colorpath const fbzcp)
{
	// clamped case is easy
	if (fbzcp.rgbzw_clamp() != 0)
		return std::clamp(s32(iterw >> 48), 0, 0xff);

	// non-clamped case has specific behaviors
	u32 result = u64(iterw) >> 48;
	if (result == 0xffff)
		return 0;
	if (result == 0x100)
		return 0xff;
	return result & 0xff;
}



/*************************************
 *
 *  Chroma keying
 *
 *************************************/

inline bool ATTR_FORCE_INLINE voodoo::voodoo_renderer::chroma_key_test(
	thread_stats_block &threadstats,
	rgbaint_t const &colorin)
{
	rgb_t color = colorin.to_rgba();

	// non-range version
	auto const chromakey = m_reg.chroma_key();
	auto const chromarange = m_reg.chroma_range();
	if (!chromarange.enable())
	{
		if (((color ^ chromakey.argb()) & 0xffffff) == 0)
		{
			threadstats.chroma_fail++;
			return false;
		}
	}

	// tricky range version
	else
	{
		s32 low, high, test;
		int results;

		// check blue
		low = chromakey.blue();
		high = chromarange.blue();
		test = color.b();
		results = (test >= low && test <= high);
		results ^= chromarange.blue_exclusive();
		results <<= 1;

		// check green
		low = chromakey.green();
		high = chromarange.green();
		test = color.g();
		results |= (test >= low && test <= high);
		results ^= chromarange.green_exclusive();
		results <<= 1;

		// check red
		low = chromakey.red();
		high = chromarange.red();
		test = color.r();
		results |= (test >= low && test <= high);
		results ^= chromarange.red_exclusive();

		// final result
		if (chromarange.union_mode())
		{
			if (results != 0)
			{
				threadstats.chroma_fail++;
				return false;
			}
		}
		else
		{
			if (results == 7)
			{
				threadstats.chroma_fail++;
				return false;
			}
		}
	}
	return true;
}



/*************************************
 *
 *  Alpha masking
 *
 *************************************/

inline bool ATTR_FORCE_INLINE voodoo::voodoo_renderer::alpha_mask_test(
	thread_stats_block &threadstats,
	u8 alpha)
{
	if ((alpha & 1) == 0)
	{
		threadstats.afunc_fail++;
		return false;
	}
	return true;
}


/*************************************
 *
 *  Alpha testing macro
 *
 *************************************/

inline bool ATTR_FORCE_INLINE voodoo::voodoo_renderer::alpha_test(
	thread_stats_block &threadstats,
	reg_alpha_mode const alphamode,
	u8 alpha)
{
	switch (alphamode.alphafunction())
	{
		case 0:     // alphaOP = never
			threadstats.afunc_fail++;
			return false;

		case 1:     // alphaOP = less than
			if (alpha >= alphamode.alpharef())
			{
				threadstats.afunc_fail++;
				return false;
			}
			break;

		case 2:     // alphaOP = equal
			if (alpha != alphamode.alpharef())
			{
				threadstats.afunc_fail++;
				return false;
			}
			break;

		case 3:     // alphaOP = less than or equal
			if (alpha > alphamode.alpharef())
			{
				threadstats.afunc_fail++;
				return false;
			}
			break;

		case 4:     // alphaOP = greater than
			if (alpha <= alphamode.alpharef())
			{
				threadstats.afunc_fail++;
				return false;
			}
			break;

		case 5:     // alphaOP = not equal
			if (alpha == alphamode.alpharef())
			{
				threadstats.afunc_fail++;
				return false;
			}
			break;

		case 6:     // alphaOP = greater than or equal
			if (alpha < alphamode.alpharef())
			{
				threadstats.afunc_fail++;
				return false;
			}
			break;

		case 7:     // alphaOP = always
			break;
	}
	return true;
}


/*************************************
 *
 *  Alpha blending macro
 *
 *************************************/

inline void ATTR_FORCE_INLINE voodoo::voodoo_renderer::alpha_blend(
	rgbaint_t &color,
	reg_fbz_mode const fbzmode,
	reg_alpha_mode const alphamode,
	s32 x,
	dither_helper const &dither,
	int dpix,
	u16 *depth,
	rgbaint_t const &prefog)
{
	// extract destination pixel
	rgbaint_t dst_color(m_rgb565[dpix]);
	int da = 0xff;
	if (fbzmode.enable_alpha_planes())
		dst_color.set_a16(da = depth[x]);

	// apply dither subtraction
	if (fbzmode.alpha_dither_subtract())
	{
		int dith = dither.subtract(x);
		dst_color.add_imm_rgba(0, dith, dith >> 1, dith);
	}

	// compute source portion
	int sa = color.get_a();
	int ta;
	rgbaint_t src_scale;
	switch (alphamode.srcrgbblend())
	{
		default:    // reserved
		case 0:     // AZERO
			src_scale.zero();
			break;

		case 1:     // ASRC_ALPHA
			ta = sa + 1;
			src_scale.set_all(ta);
			break;

		case 2:     // A_COLOR
			src_scale = dst_color;
			src_scale.add_imm(1);
			break;

		case 3:     // ADST_ALPHA
			ta = da + 1;
			src_scale.set_all(ta);
			break;

		case 4:     // AONE
			src_scale.set_all(256);
			break;

		case 5:     // AOMSRC_ALPHA
			ta = 0x100 - sa;
			src_scale.set_all(ta);
			break;

		case 6:     // AOM_COLOR
			src_scale.set_all(0x100);
			src_scale.sub(dst_color);
			break;

		case 7:     // AOMDST_ALPHA
			ta = 0x100 - da;
			src_scale.set_all(ta);
			break;

		case 15:    // ASATURATE
			ta = 0x100 - da;
			if (sa < ta)
				ta = sa;
			ta++;
			src_scale.set_all(ta);
			break;
	}

	// set src_scale alpha
	int src_alpha_scale = (alphamode.srcalphablend() == 4) ? 256 : 0;
	src_scale.set_a16(src_alpha_scale);

	// add in dest portion
	rgbaint_t dst_scale;
	switch (alphamode.dstrgbblend())
	{
		default:    // reserved
		case 0:     // AZERO
			dst_scale.zero();
			break;

		case 1:     // ASRC_ALPHA
			ta = sa + 1;
			dst_scale.set_all(ta);
			break;

		case 2:     // A_COLOR
			dst_scale.set(color);
			dst_scale.add_imm(1);
			break;

		case 3:     // ADST_ALPHA
			ta = da + 1;
			dst_scale.set_all(ta);
			break;

		case 4:     // AONE
			dst_scale.set_all(256);
			break;

		case 5:     // AOMSRC_ALPHA
			ta = 0x100 - sa;
			dst_scale.set_all(ta);
			break;

		case 6:     // AOM_COLOR
			dst_scale.set_all(0x100);
			dst_scale.sub(color);
			break;

		case 7:     // AOMDST_ALPHA
			ta = 0x100 - da;
			dst_scale.set_all(ta);
			break;

		case 15:    // A_COLORBEFOREFOG
			dst_scale.set(prefog);
			dst_scale.add_imm(1);
			break;
	}

	// set dst_scale alpha
	int dest_alpha_scale = (alphamode.dstalphablend() == 4) ? 256 : 0;
	dst_scale.set_a16(dest_alpha_scale);

	// main blend
	color.scale2_add_and_clamp(src_scale, dst_color, dst_scale);
}


/*************************************
 *
 *  Fogging macro
 *
 *************************************/

inline void ATTR_FORCE_INLINE voodoo::voodoo_renderer::apply_fogging(
	rgbaint_t &color,
	poly_extra_data const &extra,
	reg_fbz_mode const fbzmode,
	reg_fog_mode const fogmode,
	reg_fbz_colorpath const fbzcp,
	s32 x,
	dither_helper const &dither,
	s32 wfloat,
	s32 iterz,
	s64 iterw,
	rgbaint_t const &iterargb)
{
	// constant fog bypasses everything else
	rgbaint_t fog_color_local(m_reg.fog_color().argb());
	if (fogmode.fog_constant())
	{
		// if fog_mult is 0, we add this to the original color
		if (fogmode.fog_mult() == 0)
		{
			fog_color_local.add(color);
			fog_color_local.clamp_to_uint8();
		}
	}

	// non-constant fog comes from several sources
	else
	{
		s32 fogblend = 0;

		// if fog_add is zero, we start with the fog color
		if (fogmode.fog_add())
			fog_color_local.zero();

		// if fog_mult is zero, we subtract the incoming color
		// Need to check this, manual states 9 bits
		if (!fogmode.fog_mult())
			fog_color_local.sub(color);

		// fog blending mode
		switch (fogmode.fog_zalpha())
		{
			case 0:     // fog table
			{
				s32 fog_depth = wfloat;

				// add the bias for fog selection
				if (fbzmode.enable_depth_bias())
					fog_depth = std::clamp(fog_depth + s16(extra.zacolor), 0, 0xffff);

				// perform the multiply against lower 8 bits of wfloat
				s32 delta = m_fogdelta[fog_depth >> 10];
				s32 deltaval = (delta & m_fogdelta_mask) * ((fog_depth >> 2) & 0xff);

				// fog zones allow for negating this value
				if (fogmode.fog_zones() && (delta & 2))
					deltaval = -deltaval;
				deltaval >>= 6;

				// apply dither
				if (fogmode.fog_dither())
					deltaval += dither.raw_4x4(x);
				deltaval >>= 4;

				// add to the blending factor
				fogblend = m_fogblend[fog_depth >> 10] + deltaval;
				break;
			}

			case 1:     // iterated A
				fogblend = iterargb.get_a();
				break;

			case 2:     // iterated Z
				fogblend = clamped_z(iterz, fbzcp) >> 8;
				break;

			case 3:     // iterated W - Voodoo 2 only
				fogblend = clamped_w(iterw, fbzcp);
				break;
		}

		// perform the blend
		fogblend++;

		// if fog_mult is 0, we add this to the original color
		fog_color_local.scale_imm_and_clamp(s16(fogblend));
		if (fogmode.fog_mult() == 0)
		{
			fog_color_local.add(color);
			fog_color_local.clamp_to_uint8();
		}
	}

	fog_color_local.merge_alpha16(color);
	color.set(fog_color_local);
}


/*************************************
 *
 *  Pixel pipeline macros
 *
 *************************************/

inline bool ATTR_FORCE_INLINE voodoo::voodoo_renderer::stipple_test(
	thread_stats_block &threadstats,
	reg_fbz_mode const fbzmode,
	s32 x,
	s32 scry)
{
	// rotate mode
	if (fbzmode.stipple_pattern() == 0)
	{
		if (s32(m_reg.stipple_rotated()) >= 0)
		{
			threadstats.stipple_count++;
			return false;
		}
	}

	// pattern mode
	else
	{
		int stipple_index = ((scry & 3) << 3) | (~x & 7);
		if (BIT(m_reg.stipple(), stipple_index) == 0)
		{
			threadstats.stipple_count++;
			return false;
		}
	}
	return true;
}

inline s32 ATTR_FORCE_INLINE voodoo::voodoo_renderer::compute_wfloat(s64 iterw)
{
	int exp = count_leading_zeros_64(iterw) - 16;
	if (exp < 0)
		return 0x0000;
	if (exp >= 16)
		return 0xffff;
	return ((exp << 12) | ((iterw >> (35 - exp)) ^ 0x1fff)) + 1;
}

inline s32 ATTR_FORCE_INLINE voodoo::voodoo_renderer::compute_depthval(
	poly_extra_data const &extra,
	reg_fbz_mode const fbzmode,
	reg_fbz_colorpath const fbzcp,
	s32 wfloat,
	s32 iterz)
{
	s32 result;
	if (fbzmode.wbuffer_select())
	{
		if (!fbzmode.depth_float_select())
			result = wfloat;
		else if ((iterz & 0xf0000000) != 0)
			result = 0x0000;
		else if ((iterz & 0x0ffff000) == 0)
			result = 0xffff;
		else
		{
			int exp = count_leading_zeros(iterz) - 4;
			return ((exp << 12) | ((iterz >> (15 - exp)) ^ 0x1fff)) + 1;
		}
	}
	else
		result = clamped_z(iterz, fbzcp);

	if (fbzmode.enable_depth_bias())
		result = std::clamp(result + s16(extra.zacolor), 0, 0xffff);

	return result;
}


inline bool ATTR_FORCE_INLINE voodoo::voodoo_renderer::depth_test(
	thread_stats_block &threadstats,
	poly_extra_data const &extra,
	reg_fbz_mode const fbzmode,
	s32 depthdest,
	s32 depthval)
{
	// the source depth is either the iterated W/Z+bias or a constant value
	s32 depthsource = (fbzmode.depth_source_compare() == 0) ? depthval : u16(extra.zacolor);

	/* test against the depth buffer */
	switch (fbzmode.depth_function())
	{
		case 0:     /* depthOP = never */
			threadstats.zfunc_fail++;
			return false;

		case 1:     /* depthOP = less than */
			if (depthsource >= depthdest)
			{
				threadstats.zfunc_fail++;
				return false;
			}
			break;

		case 2:     /* depthOP = equal */
			if (depthsource != depthdest)
			{
				threadstats.zfunc_fail++;
				return false;
			}
			break;

		case 3:     /* depthOP = less than or equal */
			if (depthsource > depthdest)
			{
				threadstats.zfunc_fail++;
				return false;
			}
			break;

		case 4:     /* depthOP = greater than */
			if (depthsource <= depthdest)
			{
				threadstats.zfunc_fail++;
				return false;
			}
			break;

		case 5:     /* depthOP = not equal */
			if (depthsource == depthdest)
			{
				threadstats.zfunc_fail++;
				return false;
			}
			break;

		case 6:     /* depthOP = greater than or equal */
			if (depthsource < depthdest)
			{
				threadstats.zfunc_fail++;
				return false;
			}
			break;

		case 7:     /* depthOP = always */
			break;
	}
	return true;
}


inline void ATTR_FORCE_INLINE voodoo::voodoo_renderer::write_pixel(
	thread_stats_block &threadstats,
	reg_fbz_mode const fbzmode,
	dither_helper const &dither,
	u16 *destbase,
	u16 *depthbase,
	s32 x,
	rgbaint_t const &color,
	s32 depthval)
{
	/* write to framebuffer */
	if (fbzmode.rgb_buffer_mask())
		destbase[x] = dither.pixel(x, color);

	/* write to aux buffer */
	/*if (depth && FBZMODE_AUX_BUFFER_MASK(FBZMODE))*/
	if (fbzmode.aux_buffer_mask())
	{
		if (fbzmode.enable_alpha_planes() == 0)
			depthbase[x] = depthval;
		else
			depthbase[x] = color.get_a();
	}

	/* track pixel writes to the frame buffer regardless of mask */
	threadstats.pixels_out++;
}



/*************************************
 *
 *  Colorpath pipeline macro
 *
 *************************************/

/*

    c_other_is_used:

        if (fbzmode.enable_chromakey() ||
            fbzcp.cc_zero_other() == 0)

    c_local_is_used:

        if (fbzcp.cc_sub_clocal() ||
            fbzcp.cc_mselect() == 1 ||
            fbzcp.cc_add_aclocal() == 1)

    NEEDS_ITER_RGB:

        if ((c_other_is_used && fbzcp.cc_rgbselect() == 0) ||
            (c_local_is_used && (fbzcp.cc_localselect_override() != 0 || fbzcp.cc_localselect() == 0))

    NEEDS_ITER_A:

        if ((a_other_is_used && fbzcp.cc_aselect() == 0) ||
            (a_local_is_used && fbzcp.cca_localselect() == 0))

    NEEDS_ITER_Z:

        if (fbzmode.wbuffer_select() == 0 ||
            fbzmode.depth_float_select() != 0 ||
            fbzcp.cca_localselect() == 2)


*/

inline bool ATTR_FORCE_INLINE voodoo::voodoo_renderer::combine_color(
	rgbaint_t &color,
	thread_stats_block &threadstats,
	poly_extra_data const &extra,
	reg_fbz_colorpath const fbzcp,
	reg_fbz_mode const fbzmode,
	rgbaint_t texel,
	s32 iterz,
	s64 iterw)
{
	// compute c_other
	rgbaint_t c_other;
	switch (fbzcp.cc_rgbselect())
	{
		case 0:     // iterated RGB
			c_other.set(color);
			break;

		case 1:     // texture RGB
			c_other.set(texel);
			break;

		case 2:     // color1 RGB
			c_other.set(extra.color1);
			break;

		default:    // reserved - voodoo3 LFB RGB
			c_other.zero();
			break;
	}

	// handle chroma key
	if (fbzmode.enable_chromakey())
		if (!chroma_key_test(threadstats, c_other))
			return false;

	// compute a_other
	switch (fbzcp.cc_aselect())
	{
		case 0:     // iterated alpha
			c_other.merge_alpha16(color);
			break;

		case 1:     // texture alpha
			c_other.merge_alpha16(texel);
			break;

		case 2:     // color1 alpha
			c_other.set_a16(extra.color1.a());
			break;

		default:    // reserved - voodoo3  LFB Alpha
			c_other.zero_alpha();
			break;
	}

	// handle alpha mask
	if (fbzmode.enable_alpha_mask())
		if (!alpha_mask_test(threadstats, c_other.get_a()))
			return false;

	// compute c_local
	rgbaint_t c_local;
	if (fbzcp.cc_localselect_override() == 0)
	{
		if (fbzcp.cc_localselect() == 0)    // iterated RGB
			c_local.set(color);
		else                                // color0 RGB
			c_local.set(extra.color0);
	}
	else
	{
		if (!(texel.get_a() & 0x80))        // iterated RGB
			c_local.set(color);
		else                                // color0 RGB
			c_local.set(extra.color0);
	}

	// compute a_local
	switch (fbzcp.cca_localselect())
	{
		default:
		case 0:     // iterated alpha
			c_local.merge_alpha16(color);
			break;

		case 1:     // color0 alpha
			c_local.set_a16(extra.color0.a());
			break;

		case 2:     // clamped iterated Z[27:20]
			c_local.set_a16(u8(clamped_z(iterz, fbzcp) >> 8));
			break;

		case 3:     // clamped iterated W[39:32] (Voodoo 2 only)
			c_local.set_a16(u8(clamped_w(iterw, fbzcp)));
			break;
	}

	// select zero or c_other
	rgbaint_t blend_color;
	if (fbzcp.cc_zero_other())
		blend_color.zero();
	else
		blend_color.set(c_other);

	// select zero or a_other
	if (fbzcp.cca_zero_other())
		blend_color.zero_alpha();
	else
		blend_color.merge_alpha16(c_other);

	// subtract a/c_local
	if (fbzcp.cc_sub_clocal() || fbzcp.cca_sub_clocal())
	{
		rgbaint_t sub_val;

		if (!fbzcp.cc_sub_clocal())
			sub_val.zero();
		else
			sub_val.set(c_local);

		if (!fbzcp.cca_sub_clocal())
			sub_val.zero_alpha();
		else
			sub_val.merge_alpha16(c_local);

		blend_color.sub(sub_val);
	}

	// blend RGB
	rgbaint_t blend_factor;
	switch (fbzcp.cc_mselect())
	{
		default:    // reserved
		case 0:     // 0
			blend_factor.zero();
			break;

		case 1:     // c_local
			blend_factor.set(c_local);
			break;

		case 2:     // a_other
			blend_factor.set(c_other.select_alpha32());
			break;

		case 3:     // a_local
			blend_factor.set(c_local.select_alpha32());
			break;

		case 4:     // texture alpha
			blend_factor.set(texel.select_alpha32());
			break;

		case 5:     // texture RGB (Voodoo 2 only)
			blend_factor.set(texel);
			break;
	}

	// blend alpha
	switch (fbzcp.cca_mselect())
	{
		default:    // reserved
		case 0:     // 0
			blend_factor.zero_alpha();
			break;

		case 1:     // a_local
		case 3:     // a_local
			blend_factor.merge_alpha16(c_local);
			break;

		case 2:     // a_other
			blend_factor.merge_alpha16(c_other);
			break;

		case 4:     // texture alpha
			blend_factor.merge_alpha16(texel);
			break;
	}

	// reverse the RGB blend
	if (!fbzcp.cc_reverse_blend())
		blend_factor.xor_imm_rgba(0, 0xff, 0xff, 0xff);

	// reverse the alpha blend
	if (!fbzcp.cca_reverse_blend())
		blend_factor.xor_imm_rgba(0xff, 0, 0, 0);

	// add clocal or alocal to RGB
	rgbaint_t add_val;
	switch (fbzcp.cc_add_aclocal())
	{
		case 3:     // reserved
		case 0:     // nothing
			add_val.zero();
			break;

		case 1:     // add c_local
			add_val.set(c_local);
			break;

		case 2:     // add_alocal
			add_val.set(c_local.select_alpha32());
			break;
	}

	// add clocal or alocal to alpha
	if (!fbzcp.cca_add_aclocal())
		add_val.zero_alpha();
	else
		add_val.merge_alpha16(c_local);

	// add and clamp
	blend_factor.add_imm(1);
	blend_color.scale_add_and_clamp(blend_factor, add_val);

	// invert
	if (fbzcp.cca_invert_output())
		blend_color.xor_imm_rgba(0xff, 0, 0, 0);
	if (fbzcp.cc_invert_output())
		blend_color.xor_imm_rgba(0, 0xff, 0xff, 0xff);

	color.set(blend_color);
	return true;
}



inline rgb_t voodoo::raster_texture::lookup_single_texel(reg_texture_mode const texmode, u32 texbase, s32 s, s32 t)
{
	if (texmode.format() < 8)
		return m_lookup[*(u8 *)&m_ram[(texbase + t + s) & m_mask]];
	else if (texmode.format() >= 10 && texmode.format() <= 12)
		return m_lookup[*(u16 *)&m_ram[(texbase + 2*(t + s)) & m_mask]];
	else
	{
		u32 texel = *(u16 *)&m_ram[(texbase + 2*(t + s)) & m_mask];
		return (m_lookup[texel & 0xff] & 0xffffff) | ((texel & 0xff00) << 16);
	}
}

inline rgbaint_t ATTR_FORCE_INLINE voodoo::raster_texture::fetch_texel(reg_texture_mode const texmode, dither_helper const &dither, s32 x, const stw_helper &iterstw, s32 lodbase, s32 &lod)
{
	lod = lodbase;

	// determine the S/T/LOD values for this texture
	s32 s, t;
	if (texmode.enable_perspective())
	{
		s32 wlog;
		iterstw.calc_stow(s, t, wlog);
		lod += wlog;
	}
	else
		iterstw.get_st_shiftr(s, t, 14 + 10);

	// clamp W
	if (texmode.clamp_neg_w() && iterstw.is_w_neg())
		s = t = 0;

	// clamp the LOD
	lod += m_lodbias;
	if (texmode.enable_lod_dither())
		lod += dither.raw_4x4(x) << 4;
	lod = std::clamp(lod, m_lodmin, m_lodmax);

	// now the LOD is in range; if we don't own this LOD, take the next one
	s32 ilod = lod >> 8;
	if (((m_lodmask >> ilod) & 1) == 0)
		ilod++;

	// fetch the texture base
	u32 texbase = m_lodoffset[ilod];

	// compute the maximum s and t values at this LOD
	s32 smax = m_wmask >> ilod;
	s32 tmax = m_hmask >> ilod;

	// determine whether we are point-sampled or bilinear
	rgbaint_t result;
	if ((lod == m_lodmin && !texmode.magnification_filter()) || (lod != m_lodmin && !texmode.minification_filter()))
	{
		// adjust S/T for the LOD and strip off the fractions
		ilod += 18 - 10;
		s >>= ilod;
		t >>= ilod;

		// clamp/wrap S/T if necessary
		if (texmode.clamp_s())
			s = std::clamp(s, 0, smax);
		if (texmode.clamp_t())
			t = std::clamp(t, 0, tmax);
		s &= smax;
		t &= tmax;
		t *= smax + 1;

		// fetch texel data
		result.set(lookup_single_texel(texmode, texbase, s, t));
	}
	else
	{
		// adjust S/T for the LOD and strip off all but the low 8 bits of the fraction
		s >>= ilod;
		t >>= ilod;

		// also subtract 1/2 texel so that (0.5,0.5) = a full (0,0) texel
		s -= 0x80;
		t -= 0x80;

		// extract the fractions
		u32 sfrac = s & m_bilinear_mask;
		u32 tfrac = t & m_bilinear_mask;

		// now toss the rest
		s >>= 8;
		t >>= 8;
		s32 s1 = s + 1;
		s32 t1 = t + 1;

		// clamp/wrap S/T if necessary
		if (texmode.clamp_s())
		{
			if (s < 0)
				s = s1 = 0;
			else if (s >= smax)
				s = s1 = smax;
		}
		s &= smax;
		s1 &= smax;

		if (texmode.clamp_t())
		{
			if (t < 0)
				t = t1 = 0;
			else if (t >= tmax)
				t = t1 = tmax;
		}
		t &= tmax;
		t1 &= tmax;
		t *= smax + 1;
		t1 *= smax + 1;

		// fetch texel data
		u32 texel0 = lookup_single_texel(texmode, texbase, s, t);
		u32 texel1 = lookup_single_texel(texmode, texbase, s1, t);
		u32 texel2 = lookup_single_texel(texmode, texbase, s, t1);
		u32 texel3 = lookup_single_texel(texmode, texbase, s1, t1);
		result.bilinear_filter_rgbaint(texel0, texel1, texel2, texel3, sfrac, tfrac);
	}
	return result;
}

inline rgbaint_t ATTR_FORCE_INLINE voodoo::raster_texture::combine_texture(
	reg_texture_mode const texmode,
	rgbaint_t const &c_local,
	rgbaint_t const &c_other,
	s32 lod)
{
	// select zero/other for RGB
	rgbaint_t blend_color;
	if (texmode.tc_zero_other())
		blend_color.zero();
	else
		blend_color.set(c_other);

	// select zero/other for alpha
	if (texmode.tca_zero_other())
		blend_color.zero_alpha();
	else
		blend_color.merge_alpha16(c_other);

	// subtract local color
	if (texmode.tc_sub_clocal() || texmode.tca_sub_clocal())
	{
		rgbaint_t sub_val;

		// potentially subtract c_local
		if (!texmode.tc_sub_clocal())
			sub_val.zero();
		else
			sub_val.set(c_local);

		if (!texmode.tca_sub_clocal())
			sub_val.zero_alpha();
		else
			sub_val.merge_alpha16(c_local);

		blend_color.sub(sub_val);
	}

	// blend RGB
	rgbaint_t blend_factor;
	switch (texmode.tc_mselect())
	{
		default:    // reserved
		case 0:     // zero
			blend_factor.zero();
			break;

		case 1:     // c_local
			blend_factor.set(c_local);
			break;

		case 2:     // a_other
			blend_factor.set(c_other.select_alpha32());
			break;

		case 3:     // a_local
			blend_factor.set(c_local.select_alpha32());
			break;

		case 4:     // LOD (detail factor)
			if (m_detailbias <= lod)
				blend_factor.zero();
			else
			{
				u8 tmp;
				tmp = (((m_detailbias - lod) << m_detailscale) >> 8);
				if (tmp > m_detailmax)
					tmp = m_detailmax;
				blend_factor.set_all(tmp);
			}
			break;

		case 5:     // LOD fraction
			blend_factor.set_all(lod & 0xff);
			break;
	}

	// blend alpha
	switch (texmode.tca_mselect())
	{
		default:    // reserved
		case 0:     // zero
			blend_factor.zero_alpha();
			break;

		case 1:     // c_local
			blend_factor.merge_alpha16(c_local);
			break;

		case 2:     // a_other
			blend_factor.merge_alpha16(c_other);
			break;

		case 3:     // a_local
			blend_factor.merge_alpha16(c_local);
			break;

		case 4:     // LOD (detail factor)
			if (m_detailbias <= lod)
				blend_factor.zero_alpha();
			else
			{
				u8 tmp;
				tmp = (((m_detailbias - lod) << m_detailscale) >> 8);
				if (tmp > m_detailmax)
					tmp = m_detailmax;
				blend_factor.set_a16(tmp);
			}
			break;

		case 5:     // LOD fraction
			blend_factor.set_a16(lod & 0xff);
			break;
	}

	// reverse the RGB blend
	if (!texmode.tc_reverse_blend())
		blend_factor.xor_imm_rgba(0, 0xff, 0xff, 0xff);

	// reverse the alpha blend
	if (!texmode.tca_reverse_blend())
		blend_factor.xor_imm_rgba(0xff, 0, 0, 0);

	blend_factor.add_imm(1);

	// add clocal or alocal to RGB
	rgbaint_t add_val;
	switch (texmode.tc_add_aclocal())
	{
		case 3:     // reserved
		case 0:     // nothing
			add_val.zero();
			break;

		case 1:     // add c_local
			add_val.set(c_local);
			break;

		case 2:     // add_alocal
			add_val.set(c_local.select_alpha32());
			break;
	}

	// add clocal or alocal to alpha
	if (!texmode.tca_add_aclocal())
		add_val.zero_alpha();
	else
		add_val.merge_alpha16(c_local);

	// scale add and clamp
	blend_color.scale_add_and_clamp(blend_factor, add_val);

	// invert
	if (texmode.tc_invert_output())
		blend_color.xor_imm_rgba(0, 0xff, 0xff, 0xff);
	if (texmode.tca_invert_output())
		blend_color.xor_imm_rgba(0xff, 0, 0, 0);
	return blend_color;
}


/*************************************
 *
 *  Rasterizer generator macro
 *
 *************************************/

template<u32 _FbzCp, u32 _FbzMode, u32 _AlphaMode, u32 _FogMode, u32 _TexMode0, u32 _TexMode1>
void voodoo::voodoo_renderer::rasterizer(s32 y, const voodoo_renderer::extent_t &extent, const poly_extra_data &extra, int threadid)
{
	thread_stats_block &threadstats = m_thread_stats[threadid];
	reg_texture_mode const texmode0(_TexMode0, (_TexMode0 == 0xffffffff) ? 0 : reg_texture_mode(extra.u.raster.m_texmode0));
	reg_texture_mode const texmode1(_TexMode1, (_TexMode1 == 0xffffffff) ? 0 : reg_texture_mode(extra.u.raster.m_texmode1));
	reg_fbz_colorpath const fbzcp(_FbzCp, reg_fbz_colorpath(extra.u.raster.m_fbzcp));
	reg_alpha_mode const alphamode(_AlphaMode, reg_alpha_mode(extra.u.raster.m_alphamode));
	reg_fbz_mode const fbzmode(_FbzMode, reg_fbz_mode(extra.u.raster.m_fbzmode));
	reg_fog_mode const fogmode(_FogMode, reg_fog_mode(extra.u.raster.m_fogmode));
	stw_helper iterstw0, iterstw1;
	stw_helper deltastw0, deltastw1;

	// determine the screen Y
	s32 scry = y;
	if (fbzmode.y_origin())
		scry = m_yorigin - y;

	// pre-increment the pixels_in unconditionally
	s32 startx = extent.startx;
	s32 stopx = extent.stopx;
	threadstats.pixels_in += stopx - startx;

	// apply clipping
	if (fbzmode.enable_clipping())
	{
		// Y clipping buys us the whole scanline
		if (scry < m_reg.clip_top() || scry >= m_reg.clip_bottom())
		{
			threadstats.clip_fail += stopx - startx;
			return;
		}

		// X clipping
		s32 tempclip = m_reg.clip_right();

		// check for start outsize of clipping boundary
		if (startx >= tempclip)
		{
			threadstats.clip_fail += stopx - startx;
			return;
		}

		// clip the right side
		if (stopx > tempclip)
		{
			threadstats.clip_fail += stopx - tempclip;
			stopx = tempclip;
		}

		// clip the left side
		tempclip = m_reg.clip_left();
		if (startx < tempclip)
		{
			threadstats.clip_fail += tempclip - startx;
			startx = tempclip;
		}
	}

	// get pointers to the target buffer and depth buffer
	u16 *dest = extra.destbase + scry * m_rowpixels;
	u16 *depth = extra.depthbase + scry * m_rowpixels;

	// compute the starting parameters
	s32 dx = startx - (extra.ax >> 4);
	s32 dy = y - (extra.ay >> 4);
	s32 iterr = (extra.startr + dy * extra.drdy + dx * extra.drdx) << 8;
	s32 iterg = (extra.startg + dy * extra.dgdy + dx * extra.dgdx) << 8;
	s32 iterb = (extra.startb + dy * extra.dbdy + dx * extra.dbdx) << 8;
	s32 itera = (extra.starta + dy * extra.dady + dx * extra.dadx) << 8;

	rgbaint_t iterargb, iterargb_delta;
	iterargb.set(itera, iterr, iterg, iterb);
	iterargb_delta.set(extra.dadx, extra.drdx, extra.dgdx, extra.dbdx);
	iterargb_delta.shl_imm(8);
	s32 iterz = extra.startz + dy * extra.dzdy + dx * extra.dzdx;
	s64 iterw = (extra.startw + dy * extra.dwdy + dx * extra.dwdx) << 16;
	s64 iterw_delta = extra.dwdx << 16;
	if (_TexMode0 != 0xffffffff)
	{
		iterstw0.set(
			extra.starts0 + dy * extra.ds0dy + dx * extra.ds0dx,
			extra.startt0 + dy * extra.dt0dy + dx * extra.dt0dx,
			extra.startw0 + dy * extra.dw0dy + dx * extra.dw0dx);
		deltastw0.set(extra.ds0dx, extra.dt0dx, extra.dw0dx);
	}
	if (_TexMode1 != 0xffffffff)
	{
		iterstw1.set(
			extra.starts1 + dy * extra.ds1dy + dx * extra.ds1dx,
			extra.startt1 + dy * extra.dt1dy + dx * extra.dt1dx,
			extra.startw1 + dy * extra.dw1dy + dx * extra.dw1dx);
		deltastw1.set(extra.ds1dx, extra.dt1dx, extra.dw1dx);
	}
	extra.info->hits++;

	// loop in X
	dither_helper dither(scry, fbzmode, fogmode);
	for (s32 x = startx; x < stopx; x++)
	{
		do
		{
			// handle stippling
			if (fbzmode.enable_stipple() && !stipple_test(threadstats, fbzmode, x, scry))
				break;

			// compute "floating point" W value (used for depth and fog)
			s32 wfloat = compute_wfloat(iterw);

			// compute depth value (W or Z) for this pixel
			s32 depthval = compute_depthval(extra, fbzmode, fbzcp, wfloat, iterz);

			// depth testing
			if (fbzmode.enable_depthbuf() && !depth_test(threadstats, extra, fbzmode, depth[x], depthval))
				break;

			// run the texture pipeline on TMU1 to produce a value in texel
			// note that they set LOD min to 8 to "disable" a TMU
			rgbaint_t texel(0);
			if (_TexMode1 != 0xffffffff && extra.tex1->m_lodmin < (8 << 8))
			{
				s32 lod1;
				rgbaint_t texel_t1 = extra.tex1->fetch_texel(texmode1, dither, x, iterstw1, extra.lodbase1, lod1);
				texel = extra.tex1->combine_texture(texmode1, texel_t1, texel, lod1);
			}

			// run the texture pipeline on TMU0 to produce a final result in texel
			// note that they set LOD min to 8 to "disable" a TMU
			if (_TexMode0 != 0xffffffff && extra.tex0->m_lodmin < (8 << 8))
			{
//				if (!m_send_config)
				{
					s32 lod0;
					rgbaint_t texel_t0 = extra.tex0->fetch_texel(texmode0, dither, x, iterstw0, extra.lodbase0, lod0);
					texel = extra.tex0->combine_texture(texmode0, texel_t0, texel, lod0);
				}
//				else
//					texel.set(m_tmu_config);
			}

			// colorpath pipeline selects source colors and does blending
			rgbaint_t color = clamped_argb(iterargb, fbzcp);
			if (!combine_color(color, threadstats, extra, fbzcp, fbzmode, texel, iterz, iterw))
				break;

			// handle alpha test
			if (alphamode.alphatest() && !alpha_test(threadstats, alphamode, color.get_a()))
				break;

			// perform fogging
			rgbaint_t prefog(color);
			if (fogmode.enable_fog())
				apply_fogging(color, extra, fbzmode, fogmode, fbzcp, x, dither, wfloat, iterz, iterw, iterargb);

			// perform alpha blending
			if (alphamode.alphablend())
				alpha_blend(color, fbzmode, alphamode, x, dither, dest[x], depth, prefog);

			// store the pixel and depth value
			write_pixel(threadstats, fbzmode, dither, dest, depth, x, color, depthval);
		} while (0);

		// update the iterated parameters
		iterargb += iterargb_delta;
		iterz += extra.dzdx;
		iterw += iterw_delta;
		if (_TexMode0 != 0xffffffff)
			iterstw0.add(deltastw0);
		if (_TexMode1 != 0xffffffff)
			iterstw1.add(deltastw1);
	}
}

/***************************************************************************
    GENERIC RASTERIZERS
***************************************************************************/

/*-------------------------------------------------
    raster_fastfill - per-scanline
    implementation of the 'fastfill' command
-------------------------------------------------*/

void voodoo::voodoo_renderer::raster_fastfill(s32 y, const voodoo_renderer::extent_t &extent, const poly_extra_data &extra, int threadid)
{
	thread_stats_block &threadstats = m_thread_stats[threadid];
	auto const fbzmode = m_reg.fbz_mode();
	s32 startx = extent.startx;
	s32 stopx = extent.stopx;
	int x;

	/* determine the screen Y */
	s32 scry = y;
	if (fbzmode.y_origin())
		scry = m_yorigin - y;

	/* fill this RGB row */
	if (fbzmode.rgb_buffer_mask())
	{
		const u16 *ditherow = &extra.u.dither[(y & 3) * 4];
		u64 expanded = *(u64 *)ditherow;
		u16 *dest = extra.destbase + scry * m_rowpixels;

		for (x = startx; x < stopx && (x & 3) != 0; x++)
			dest[x] = ditherow[x & 3];
		for ( ; x < (stopx & ~3); x += 4)
			*(u64 *)&dest[x] = expanded;
		for ( ; x < stopx; x++)
			dest[x] = ditherow[x & 3];
		threadstats.pixels_out += stopx - startx;
	}

	/* fill this dest buffer row */
	if (fbzmode.aux_buffer_mask() && extra.depthbase != nullptr)
	{
		u16 depth = m_reg.za_color();
		u64 expanded = (u64(depth) << 48) | (u64(depth) << 32) | (u64(depth) << 16) | u64(depth);
		u16 *dest = extra.depthbase + scry * m_rowpixels;

		for (x = startx; x < stopx && (x & 3) != 0; x++)
			dest[x] = depth;
		for ( ; x < (stopx & ~3); x += 4)
			*(u64 *)&dest[x] = expanded;
		for ( ; x < stopx; x++)
			dest[x] = depth;
	}
}


void voodoo_renderer::pixel_pipeline(thread_stats_block &threadstats, poly_extra_data const &extra, voodoo::reg_lfb_mode const lfbmode, s32 x, s32 scry, rgb_t src_color, u16 sz)
{
	reg_fbz_colorpath const fbzcp(extra.u.raster.m_fbzcp);
	reg_alpha_mode const alphamode(extra.u.raster.m_alphamode);
	reg_fbz_mode const fbzmode(extra.u.raster.m_fbzmode);
	reg_fog_mode const fogmode(extra.u.raster.m_fogmode);
	dither_helper dither(scry, fbzmode, fogmode);
	u16 *depth = extra.depthbase;
	u16 *dest = extra.destbase;

	threadstats.pixels_in++;

	// apply clipping
	if (fbzmode.enable_clipping())
	{
		if (x < m_reg.clip_left() || x >= m_reg.clip_right() || scry < m_reg.clip_top() || scry >= m_reg.clip_bottom())
		{
			threadstats.clip_fail++;
			return;
		}
	}

	// handle stippling
	if (fbzmode.enable_stipple() && !stipple_test(threadstats, fbzmode, x, scry))
		return;

	// Depth testing value for lfb pipeline writes is directly from write data, no biasing is used
	s32 depthval = u32(sz);

	// Perform depth testing
	if (fbzmode.enable_depthbuf() && !depth_test(threadstats, extra, fbzmode, depth[x], depthval))
		return;

	// use the RGBA we stashed above
	rgbaint_t color(src_color);

	// handle chroma key
	if (fbzmode.enable_chromakey() && !chroma_key_test(threadstats, color))
		return;

	// handle alpha mask
	if (fbzmode.enable_alpha_mask() && !alpha_mask_test(threadstats, color.get_a()))
		return;

	// handle alpha test
	if (alphamode.alphatest() && !alpha_test(threadstats, alphamode, color.get_a()))
		return;

	// perform fogging
	rgbaint_t prefog(color);
	if (fogmode.enable_fog())
	{
		s32 iterz = sz << 12;
		s64 iterw = lfbmode.write_w_select() ? u32(extra.zacolor << 16) : u32(sz << 16);
		apply_fogging(color, extra, fbzmode, fogmode, fbzcp, x, dither, depthval, iterz, iterw, rgbaint_t(0));
	}

	// wait for any outstanding work to finish
	wait("LFB Write");

	// perform alpha blending
	if (alphamode.alphablend())
		alpha_blend(color, fbzmode, alphamode, x, dither, dest[x], depth, prefog);

	// pixel pipeline part 2 handles final output
	write_pixel(threadstats, fbzmode, dither, dest, depth, x, color, depthval);
}



}

/*************************************
*
*  Rasterizer table
*
*************************************/

using namespace voodoo;

#define RASTERIZER_ENTRY(fbzcp, alpha, fog, fbz, tex0, tex1) \
	{ &voodoo_renderer::rasterizer<fbzcp, fbz, alpha, fog, tex0, tex1>, { fbzcp, alpha, fog, fbz, tex0, tex1 } },

voodoo::static_raster_info voodoo_device::predef_raster_table[] =
{
#include "voodoo_rast.ipp"
	{ nullptr, { 0xffffffff } }
};

#undef RASTERIZER_ENTRY

voodoo::static_raster_info voodoo_device::generic_raster_table[3] =
{
	{ &voodoo_renderer::rasterizer<reg_fbz_colorpath::DECODE_LIVE, reg_fbz_mode::DECODE_LIVE, reg_alpha_mode::DECODE_LIVE, reg_fog_mode::DECODE_LIVE, 0xffffffff, 0xffffffff>, { 0 } },
	{ &voodoo_renderer::rasterizer<reg_fbz_colorpath::DECODE_LIVE, reg_fbz_mode::DECODE_LIVE, reg_alpha_mode::DECODE_LIVE, reg_fog_mode::DECODE_LIVE, reg_texture_mode::DECODE_LIVE, 0xffffffff>, { 0 } },
	{ &voodoo_renderer::rasterizer<reg_fbz_colorpath::DECODE_LIVE, reg_fbz_mode::DECODE_LIVE, reg_alpha_mode::DECODE_LIVE, reg_fog_mode::DECODE_LIVE, reg_texture_mode::DECODE_LIVE, reg_texture_mode::DECODE_LIVE>, { 0 } },
};


