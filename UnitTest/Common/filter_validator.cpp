#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>

#include "Common/align.h"
#include "Common/pixel.h"
#include "Common/zfilter.h"

#include "gtest/gtest.h"

extern "C" {
	#include "sha1/sha1.h"
}

#include "audit_buffer.h"
#include "filter_validator.h"

namespace {;

void decode_sha1(const char *str, unsigned char digest[20])
{
	for (unsigned i = 0; i < 20; ++i) {
		char buf[3] = { str[i * 2], str[i * 2 + 1], '\0' };
		digest[i] = static_cast<unsigned char>(std::stoi(buf, nullptr, 16));
	}
}

template <class T>
void hash_buffer(const AuditBuffer<T> &buf, unsigned p, unsigned width, unsigned height, unsigned char digest[20])
{
	const zimg::ZimgImageBufferConst &image_buffer = buf.as_image_buffer();
	SHA1_CTX sha_ctx;

	SHA1Init(&sha_ctx);

	for (unsigned i = 0; i < height; ++i) {
		const unsigned char *ptr = (const unsigned char *)image_buffer.data[p] + (ptrdiff_t)i * image_buffer.stride[p];
		SHA1Update(&sha_ctx, ptr, width * sizeof(T));
	}

	SHA1Final(digest, &sha_ctx);
}

std::string hash_to_str(const unsigned char digest[20])
{
	std::string s;
	s.reserve(40);

	for (unsigned i = 0; i < 20; ++i) {
		char x[3];

		sprintf(x, "%02x", digest[i]);

		s.push_back(x[0]);
		s.push_back(x[1]);
	}

	return s;
}


void validate_flags(const zimg::IZimgFilter *filter)
{
	auto flags = filter->get_flags();

	ASSERT_EQ(zimg::API_VERSION, flags.version);

	if (flags.entire_plane && !flags.entire_row)
		FAIL() << "filter must set entire_row if entire_plane is set";
	if (flags.in_place && !flags.same_row)
		FAIL() << "filter must set same_row if in_place is set";

	if (flags.entire_plane && filter->get_max_buffering() != (unsigned)-1)
		FAIL() << "filter must buffer entire image if entire_plane is set";
	if (flags.entire_plane && filter->get_simultaneous_lines() != (unsigned)-1)
		FAIL() << "filter must output entire image if entire_plane is set";
}

void validate_same_row(const zimg::IZimgFilter *filter)
{
	auto flags = filter->get_flags();
	auto attr = filter->get_image_attributes();

	unsigned fstep = filter->get_simultaneous_lines();
	unsigned step = flags.has_state ? fstep : 1;

	for (unsigned i = 0; i < attr.height; i += step) {
		auto range = filter->get_required_row_range(i);
		ASSERT_EQ(i, range.first);
		ASSERT_EQ(i + fstep, range.second);
	}
}

template <class T, class U>
void validate_filter_plane(const zimg::IZimgFilter *filter, AuditBuffer<T> *src_buffer, AuditBuffer<U> *dst_buffer)
{
	auto attr = filter->get_image_attributes();

	unsigned step = filter->get_simultaneous_lines();
	zimg::AlignedVector<char> ctx(filter->get_context_size());
	zimg::AlignedVector<char> tmp(filter->get_tmp_size(0, attr.width));

	filter->init_context(ctx.data());

	for (unsigned i = 0; i < attr.height; i += step) {
		filter->process(ctx.data(), src_buffer->as_image_buffer(), dst_buffer->as_image_buffer(), tmp.data(), i, 0, attr.width);

		for (unsigned ii = i; ii < std::min(i + step, attr.height); ++ii) {
			ASSERT_TRUE(dst_buffer->detect_write(ii, 0, attr.width)) <<
				"expected write to buffer at line: " << ii;
		}
		for (unsigned ii = i + step; ii < attr.height; ++ii) {
			ASSERT_FALSE(dst_buffer->detect_write(ii, 0, attr.width)) <<
				"unexpected write to buffer at line: " << ii;
		}
	}

	src_buffer->assert_guard_bytes();
	dst_buffer->assert_guard_bytes();
}

template <class T, class U>
void validate_filter_buffered(const zimg::IZimgFilter *filter, unsigned src_width, unsigned src_height, const zimg::PixelFormat &src_format, const AuditBuffer<U> &ref_buffer)
{
	auto flags = filter->get_flags();
	auto attr = filter->get_image_attributes();

	AuditBuffer<T> src_buf{ src_width, src_height, src_format, filter->get_max_buffering(), 0, 0, !!flags.color };
	AuditBuffer<U> dst_buf{ attr.width, attr.height, zimg::default_pixel_format(attr.type), filter->get_simultaneous_lines(), 0, 0, !!flags.color };

	unsigned init = flags.has_state ? 0 : attr.height / 4;
	unsigned vstep = filter->get_simultaneous_lines();
	unsigned step = flags.has_state ? vstep : vstep * 2;

	unsigned left = flags.entire_row ? 0 : attr.width / 4;
	unsigned right = flags.entire_row ? attr.width : attr.width / 2;

	zimg::AlignedVector<char> ctx(filter->get_context_size());
	zimg::AlignedVector<char> tmp(filter->get_tmp_size(left, right));

	auto col_range = filter->get_required_col_range(left, right);

	for (unsigned i = init; i < attr.height; i += step) {
		auto row_range = filter->get_required_row_range(i);

		src_buf.default_fill();
		src_buf.random_fill(row_range.first, row_range.second, col_range.first, col_range.second);
		dst_buf.default_fill();

		filter->process(ctx.data(), src_buf.as_image_buffer(), dst_buf.as_image_buffer(), tmp.data(), i, left, right);

		src_buf.assert_guard_bytes();
		dst_buf.assert_guard_bytes();

		for (unsigned ii = i; ii < std::min(i + vstep, attr.height); ++ii) {
			dst_buf.assert_eq(ref_buffer, ii, left, right);
		}
	}
}

template <class T, class U>
void validate_filter_T(const zimg::IZimgFilter *filter, unsigned src_width, unsigned src_height, const zimg::PixelFormat &src_format, const char * const sha1_str[3])
{
	zimg::ZimgFilterFlags flags = filter->get_flags();
	auto attr = filter->get_image_attributes();

	validate_flags(filter);

	if (flags.same_row)
		validate_same_row(filter);

	AuditBuffer<T> src_buf{ src_width, src_height, src_format, (unsigned)-1, 0, 0, !!flags.color };
	AuditBuffer<U> dst_buf{ attr.width, attr.height, zimg::default_pixel_format(attr.type), (unsigned)-1, 0, 0, !!flags.color };

	src_buf.random_fill(0, src_height, 0, src_width);
	dst_buf.default_fill();

	validate_filter_plane(filter, &src_buf, &dst_buf);

	if (sha1_str) {
		for (unsigned p = 0; p < (flags.color ? 3U : 1U); ++p) {
			std::array<unsigned char, 20> expected_sha1;
			std::array<unsigned char, 20> test_sha1;

			if (sha1_str[p]) {
				decode_sha1(sha1_str[p], expected_sha1.data());
				hash_buffer(dst_buf, p, attr.width, attr.height, test_sha1.data());

				EXPECT_TRUE(expected_sha1 == test_sha1) <<
					"sha1 mismatch: plane (" << p << ") expected (" << hash_to_str(expected_sha1.data()) << ") actual (" << hash_to_str(test_sha1.data()) << ")";
			}
		}
	}

	if (!flags.entire_plane)
		validate_filter_buffered<T, U>(filter, src_width, src_height, src_format, dst_buf);
}

} // namespace


void validate_filter(const zimg::IZimgFilter *filter, unsigned src_width, unsigned src_height, zimg::PixelType src_type, const char * const sha1_str[3])
{
	validate_filter(filter, src_width, src_height, zimg::default_pixel_format(src_type), sha1_str);
}

void validate_filter(const zimg::IZimgFilter *filter, unsigned src_width, unsigned src_height, const zimg::PixelFormat &src_format, const char * const sha1_str[3])
{
	zimg::PixelType src_type = src_format.type;
	auto attr = filter->get_image_attributes();

	if (src_type == zimg::PixelType::BYTE) {
		if (attr.type == zimg::PixelType::BYTE)
			validate_filter_T<uint8_t, uint8_t>(filter, src_width, src_height, src_format, sha1_str);
		else if (attr.type == zimg::PixelType::WORD || attr.type == zimg::PixelType::HALF)
			validate_filter_T<uint8_t, uint16_t>(filter, src_width, src_height, src_format, sha1_str);
		else
			validate_filter_T<uint8_t, float>(filter, src_width, src_height, src_format, sha1_str);
	} else if (src_type == zimg::PixelType::WORD || src_type == zimg::PixelType::HALF) {
		if (attr.type == zimg::PixelType::BYTE)
			validate_filter_T<uint16_t, uint8_t>(filter, src_width, src_height, src_format, sha1_str);
		else if (attr.type == zimg::PixelType::WORD || attr.type == zimg::PixelType::HALF)
			validate_filter_T<uint16_t, uint16_t>(filter, src_width, src_height, src_format, sha1_str);
		else
			validate_filter_T<uint16_t, float>(filter, src_width, src_height, src_format, sha1_str);
	} else {
		if (attr.type == zimg::PixelType::BYTE)
			validate_filter_T<float, uint8_t>(filter, src_width, src_height, src_format, sha1_str);
		else if (attr.type == zimg::PixelType::WORD || attr.type == zimg::PixelType::HALF)
			validate_filter_T<float, uint16_t>(filter, src_width, src_height, src_format, sha1_str);
		else
			validate_filter_T<float, float>(filter, src_width, src_height, src_format, sha1_str);
	}
}
