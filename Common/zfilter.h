#pragma once

#ifndef ZIMG_ZFILTER_H_
#define ZIMG_ZFILTER_H_

#include <cstddef>
#include <limits>
#include <utility>
#include "align.h"
#include "ztypes.h"

struct zimg_filter {
	virtual inline ~zimg_filter() = 0;
};

zimg_filter::~zimg_filter()
{
}


namespace zimg {;

enum class PixelType;

class IZimgFilter : public zimg_filter {
public:
	struct image_attributes {
		unsigned width;
		unsigned height;
		PixelType type;
	};

	typedef std::pair<unsigned, unsigned> pair_unsigned;

	virtual inline ~IZimgFilter() = 0;

	virtual ZimgFilterFlags get_flags() const = 0;

	virtual image_attributes get_image_attributes() const = 0;

	virtual pair_unsigned get_required_row_range(unsigned i) const = 0;

	virtual pair_unsigned get_required_col_range(unsigned left, unsigned right) const = 0;

	virtual unsigned get_simultaneous_lines() const = 0;

	virtual unsigned get_max_buffering() const = 0;

	virtual size_t get_context_size() const = 0;

	virtual size_t get_tmp_size(unsigned left, unsigned right) const = 0;

	virtual void init_context(void *ctx) const = 0;

	virtual void process(void *ctx, const ZimgImageBufferConst &src, const ZimgImageBuffer &dst, void *tmp, unsigned i, unsigned left, unsigned right) const = 0;
};

class ZimgFilter : public IZimgFilter {
public:
	virtual inline ~ZimgFilter() = 0;

	pair_unsigned get_required_row_range(unsigned i) const override
	{
		return{ i, i + 1 };
	}

	pair_unsigned get_required_col_range(unsigned left, unsigned right) const override
	{
		return{ left, right };
	}

	unsigned get_simultaneous_lines() const override
	{
		return 1;
	}

	unsigned get_max_buffering() const override
	{
		return 1;
	}

	size_t get_context_size() const override
	{
		return 0;
	}

	size_t get_tmp_size(unsigned, unsigned) const override
	{
		return 0;
	}

	void init_context(void *ctx) const override
	{
	}
};

IZimgFilter::~IZimgFilter()
{
}

ZimgFilter::~ZimgFilter()
{
}

inline bool operator==(const IZimgFilter::image_attributes &a, const IZimgFilter::image_attributes &b)
{
	return a.width == b.width && a.height == b.height && a.type == b.type;
}

inline bool operator!=(const IZimgFilter::image_attributes &a, const IZimgFilter::image_attributes &b)
{
	return !operator==(a, b);
}

inline unsigned select_zimg_buffer_mask(unsigned count)
{
	const unsigned UINT_BITS = std::numeric_limits<unsigned>::digits;

	if (count != 0 && ((count - 1) & (1 << (UINT_BITS - 1))))
		return -1;

	for (unsigned i = UINT_BITS - 1; i != 0; --i) {
		if ((count - 1) & (1 << (i - 1)))
			return (1 << i) - 1;
	}

	return 0;
}

} // namespace zimg

#endif // ZIMG_ZFILTER_H_
