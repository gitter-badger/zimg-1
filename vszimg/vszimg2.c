#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
  #include <Windows.h>
  typedef CRITICAL_SECTION vszimg_mutex_t;

  static int vszimg_mutex_init(vszimg_mutex_t *mutex) { InitializeCriticalSection(mutex); return 0;  }
  static int vszimg_mutex_lock(vszimg_mutex_t *mutex) { EnterCriticalSection(mutex); return 0; }
  static int vszimg_mutex_unlock(vszimg_mutex_t *mutex) { LeaveCriticalSection(mutex); return 0; }
  static int vszimg_mutex_destroy(vszimg_mutex_t *mutex) { DeleteCriticalSection(mutex); return 0; }
#else
  #include <pthread.h>

  typedef pthread_mutex_t vszimg_mutex_t;
  static int vszimg_mutex_init(vszimg_mutex_t *mutex) { return pthread_mutex_init(mutex, NULL); }
  static int vszimg_mutex_lock(vszimg_mutex_t *mutex) { return pthread_mutex_lock(mutex); }
  static int vszimg_mutex_unlock(vszimg_mutex_t *mutex) { return pthread_mutex_unlock(mutex); }
  static int vszimg_mutex_destroy(vszimg_mutex_t *mutex) { return pthread_mutex_destroy(mutex); }
#endif // _WIN32

#include <zimg3.h>

#if ZIMG_API_VERSION < 2
  #error zAPI v2 or greater required
#endif

#include "VapourSynth.h"
#include "VSHelper.h"

typedef unsigned char vszimg_bool;
#define VSZIMG_TRUE  1
#define VSZIMG_FALSE 0

static unsigned g_version_info[3];
static unsigned g_api_version;


struct string_table_entry {
	const char *str;
	int val;
};


#define ARRAY_SIZE(x) sizeof((x)) / sizeof((x)[0])

const struct string_table_entry g_cpu_type_table[] = {
	{ "none",  ZIMG_CPU_NONE },
	{ "auto",  ZIMG_CPU_AUTO },
#if defined(__i386) || defined(_M_IX86) || defined(_M_X64) || defined(__x86_64__)
	{ "mmx",   ZIMG_CPU_X86_MMX },
	{ "sse",   ZIMG_CPU_X86_SSE },
	{ "sse2",  ZIMG_CPU_X86_SSE2 },
	{ "sse3",  ZIMG_CPU_X86_SSE3 },
	{ "ssse3", ZIMG_CPU_X86_SSSE3 },
	{ "sse41", ZIMG_CPU_X86_SSE41 },
	{ "sse42", ZIMG_CPU_X86_SSE42 },
	{ "avx",   ZIMG_CPU_X86_AVX },
	{ "f16c",  ZIMG_CPU_X86_F16C },
	{ "avx2",  ZIMG_CPU_X86_AVX2 },
#endif
};

const struct string_table_entry g_range_table[] = {
	{ "limited", ZIMG_RANGE_LIMITED },
	{ "full",    ZIMG_RANGE_FULL },
};

const struct string_table_entry g_chromaloc_table[] = {
	{ "left",        ZIMG_CHROMA_LEFT },
	{ "center",      ZIMG_CHROMA_CENTER },
	{ "top_left",    ZIMG_CHROMA_TOP_LEFT },
	{ "top",         ZIMG_CHROMA_TOP },
	{ "bottom_left", ZIMG_CHROMA_BOTTOM_LEFT },
	{ "bottom",      ZIMG_CHROMA_BOTTOM },
};

const struct string_table_entry g_matrix_table[] = {
	{ "rgb",     ZIMG_MATRIX_RGB },
	{ "709",     ZIMG_MATRIX_709 },
	{ "unspec",  ZIMG_MATRIX_UNSPECIFIED },
	{ "470bg",   ZIMG_MATRIX_470BG },
	{ "170m",    ZIMG_MATRIX_170M },
	{ "2020ncl", ZIMG_MATRIX_2020_NCL },
	{ "2020cl",  ZIMG_MATRIX_2020_CL },
};

const struct string_table_entry g_transfer_table[] = {
	{ "709",     ZIMG_TRANSFER_709 },
	{ "unspec",  ZIMG_TRANSFER_UNSPECIFIED },
	{ "601",     ZIMG_TRANSFER_601 },
	{ "linear",  ZIMG_TRANSFER_LINEAR },
	{ "2020_10", ZIMG_TRANSFER_2020_10 },
	{ "2020_12", ZIMG_TRANSFER_2020_12 },
};

const struct string_table_entry g_primaries_table[] = {
	{ "709",    ZIMG_PRIMARIES_709 },
	{ "unspec", ZIMG_PRIMARIES_UNSPECIFIED },
	{ "170m",   ZIMG_PRIMARIES_170M },
	{ "240m",   ZIMG_PRIMARIES_240M },
	{ "2020",   ZIMG_PRIMARIES_2020 },
};

const struct string_table_entry g_dither_type_table[] = {
	{ "none",            ZIMG_DITHER_NONE },
	{ "ordered",         ZIMG_DITHER_ORDERED },
	{ "random",          ZIMG_DITHER_RANDOM },
	{ "error_diffusion", ZIMG_DITHER_ERROR_DIFFUSION }
};

const struct string_table_entry g_resample_filter_table[] = {
	{ "point",    ZIMG_RESIZE_POINT },
	{ "bilinear", ZIMG_RESIZE_BILINEAR },
	{ "bicubic",  ZIMG_RESIZE_BICUBIC },
	{ "spline16", ZIMG_RESIZE_SPLINE16 },
	{ "spline36", ZIMG_RESIZE_SPLINE36 },
	{ "lanczos",  ZIMG_RESIZE_LANCZOS }
};


static int table_lookup_str(const struct string_table_entry *table, size_t n, const char *str, int *out)
{
	size_t i = 0;

	for (i = 0; i < n; ++i) {
		if (!strcmp(table[i].str, str)) {
			*out = table[i].val;
			return 0;
		}
	}
	return 1;
}

static int propGetUintDef(const VSAPI *vsapi, const VSMap *map, const char *key, int *out, int def)
{
	int err = 0;
	int64_t x;

	if ((x = vsapi->propGetInt(map, key, 0, &err)), err)
		x = def;

	if (x < INT_MIN || x > INT_MAX) {
		return 1;
	} else {
		*out = (unsigned)x;
		return 0;
	}
}

static int propGetSintDef(const VSAPI *vsapi, const VSMap *map, const char *key, int *out, int def)
{
	int err = 0;
	int64_t x;

	if ((x = vsapi->propGetInt(map, key, 0, &err)), err)
		x = def;

	if (x < 0 || x > INT_MAX) {
		return 1;
	} else {
		*out = (int)x;
		return 0;
	}
}

static double propGetFloatDef(const VSAPI *vsapi, const VSMap *map, const char *key, double def)
{
	int err = 0;
	double x;

	if ((x = vsapi->propGetFloat(map, key, 0, &err)), err)
		x = def;

	return x;
}

static int tryGetEnumStr(const VSAPI *vsapi, const VSMap *map, const char *key, int *out, vszimg_bool *flag, const struct string_table_entry *table, size_t table_size)
{
	int err = 0;
	const char *enum_str;

	if ((enum_str = vsapi->propGetData(map, key, 0, &err)), err) {
		*flag = VSZIMG_FALSE;
		return 0;
	}

	if (table_lookup_str(table, table_size, enum_str, out)) {
		return 1;
	} else {
		*flag = VSZIMG_TRUE;
		return 0;
	}
}

static int tryGetEnum(const VSAPI *vsapi, const VSMap *map, const char *key, int *out, vszimg_bool *flag, const struct string_table_entry *table, size_t table_size)
{
	char key_str[64];
	int64_t enum_int;
	int err = 0;

	if ((enum_int = vsapi->propGetInt(map, key, 0, &err)), !err) {
		if (enum_int < INT_MIN || enum_int > INT_MAX) {
			return 1;
		} else {
			*out = (int)enum_int;
			*flag = VSZIMG_TRUE;
			return 0;
		}
	} else {
		sprintf(key_str, "%s_s", key);
		return tryGetEnumStr(vsapi, map, key_str, out, flag, table, table_size);
	}
}


static int translate_pixel_type(const VSFormat *format, zimg_pixel_type_e *out)
{
	if (format->sampleType == stInteger && format->bytesPerSample == 1)
		*out = ZIMG_PIXEL_BYTE;
	else if (format->sampleType == stInteger && format->bytesPerSample == 2)
		*out = ZIMG_PIXEL_WORD;
	else if (format->sampleType == stFloat && format->bytesPerSample == 2)
		*out = ZIMG_PIXEL_HALF;
	else if (format->sampleType == stFloat && format->bytesPerSample == 4)
		*out = ZIMG_PIXEL_FLOAT;
	else
		return 1;

	return 0;
}

static int translate_color_family(VSColorFamily cf, zimg_color_family_e *out, zimg_matrix_coefficients_e *out_matrix)
{
	switch (cf) {
	case cmGray:
		*out = ZIMG_COLOR_GREY;
		*out_matrix = ZIMG_MATRIX_UNSPECIFIED;
		return 0;
	case cmRGB:
		*out = ZIMG_COLOR_RGB;
		*out_matrix = ZIMG_MATRIX_RGB;
		return 0;
	case cmYUV:
	case cmYCoCg:
		*out = ZIMG_COLOR_YUV;
		*out_matrix = ZIMG_MATRIX_UNSPECIFIED;
		return 0;
	default:
		return 1;
	}
}

static int translate_vsformat(const VSFormat *vsformat, zimg_image_format *format, char *err_msg)
{
#define FAIL(msg) \
  do { \
    sprintf(err_msg, (msg)); \
    return 1; \
  } while (0)

	if (translate_pixel_type(vsformat, &format->pixel_type))
		FAIL("incompatible pixel type");

	format->subsample_w = vsformat->subSamplingW;
	format->subsample_h = vsformat->subSamplingH;

	if (translate_color_family(vsformat->colorFamily, &format->color_family, &format->matrix_coefficients))
		FAIL("incompatible color family");

	format->depth = vsformat->bitsPerSample;
	format->pixel_range = (format->color_family == ZIMG_COLOR_RGB) ? ZIMG_RANGE_FULL : ZIMG_RANGE_LIMITED;

	format->field_parity = ZIMG_FIELD_PROGRESSIVE;
	format->chroma_location = (format->subsample_w || format->subsample_h) ? ZIMG_CHROMA_LEFT : ZIMG_CHROMA_CENTER;

	return 0;
#undef FAIL
}

static vszimg_bool image_format_eq(const zimg_image_format *a, const zimg_image_format *b)
{
	vszimg_bool ret = VSZIMG_TRUE;

	ret = ret && a->width == b->width;
	ret = ret && a->height == b->height;
	ret = ret && a->pixel_type == b->pixel_type;
	ret = ret && a->subsample_w == b->subsample_w;
	ret = ret && a->subsample_h == b->subsample_h;
	ret = ret && a->color_family == b->color_family;

	if (a->color_family != ZIMG_COLOR_GREY) {
		ret = ret && a->matrix_coefficients == b->matrix_coefficients;
		ret = ret && a->transfer_characteristics == b->transfer_characteristics;
		ret = ret && a->color_primaries == b->color_primaries;
	}

	ret = ret && a->depth == b->depth;
	ret = ret && a->pixel_range == b->pixel_range;
	ret = ret && a->field_parity == b->field_parity;

	if (a->color_family == ZIMG_COLOR_YUV && (a->subsample_w || a->subsample_h))
		ret = ret && a->chroma_location == b->chroma_location;

	return ret;
}

static void format_zimg_error(char *err_msg, size_t n)
{
	zimg_error_code_e err_code;
	int offset;

	err_code = zimg_get_last_error(NULL, 0);
	offset = sprintf(err_msg, "zimg %d: ", err_code);
	zimg_get_last_error(err_msg + offset, n - offset);
}

static int import_frame_props(const VSAPI *vsapi, const VSMap *props, zimg_image_format *format, char *err_msg)
{
#define FAIL(name) \
  do { \
    sprintf(err_msg, "%s: bad value", (name)); \
	return 1; \
  } while (0)
#define COPY_INT_POSITIVE(name, out, invalid) \
  do { \
    int tmp; \
    if (propGetSintDef(vsapi, props, name, &tmp, out)) \
	  FAIL(name); \
    if (tmp >= 0 && tmp != invalid) \
      (out) = tmp; \
  } while (0)

	int64_t tmp_i64;
	int err = 0;

	COPY_INT_POSITIVE("_ChromaLocation", format->chroma_location, -1);

	if ((tmp_i64 = vsapi->propGetInt(props, "_ColorRange", 0, &err)), !err) {
		if (tmp_i64 == 0)
			format->pixel_range = ZIMG_RANGE_FULL;
		else if (tmp_i64 == 1)
			format->pixel_range = ZIMG_RANGE_LIMITED;
		else
			FAIL("_ColorRange");
	}

	/* Ignore UNSPECIFIED values from properties, since the user can specify them. */
	COPY_INT_POSITIVE("_Matrix", format->matrix_coefficients, ZIMG_MATRIX_UNSPECIFIED);
	COPY_INT_POSITIVE("_Transfer", format->transfer_characteristics, ZIMG_TRANSFER_UNSPECIFIED);
	COPY_INT_POSITIVE("_Primaries", format->color_primaries, ZIMG_PRIMARIES_UNSPECIFIED);

	if ((tmp_i64 = vsapi->propGetInt(props, "_FieldBased", 0, &err)), !err) {
		if (tmp_i64 != 0)
			FAIL("_FieldBased");
	}

	return 0;
#undef FAIL
#undef COPY_INT_POSITIVE
}

static void export_frame_props(const VSAPI *vsapi, const zimg_image_format *format, VSMap *props)
{
#define COPY_INT_POSITIVE(name, val) \
  do {; \
    if (val >= 0) \
	  vsapi->propSetInt(props, (name), (val), paReplace); \
    else \
	  vsapi->propDeleteKey(props, (name)); \
  } while (0)

	char version_str[64];

	sprintf(version_str, "%u.%u.%u", g_version_info[0], g_version_info[1], g_version_info[2]);

	vsapi->propSetInt(props, "ZimgApiVersion", g_api_version, paReplace);
	vsapi->propSetData(props, "ZimgVersion", version_str, (int)strlen(version_str) + 1, paReplace);

	COPY_INT_POSITIVE("_ChromaLocation", format->chroma_location);

	if (format->pixel_range == ZIMG_RANGE_FULL)
		vsapi->propSetInt(props, "_ColorRange", 0, paReplace);
	else if (format->pixel_range == ZIMG_RANGE_LIMITED)
		vsapi->propSetInt(props, "_ColorRange", 1, paReplace);
	else
		vsapi->propDeleteKey(props, "_ColorRange");

	COPY_INT_POSITIVE("_Matrix", format->matrix_coefficients);
	COPY_INT_POSITIVE("_Transfer", format->transfer_characteristics);
	COPY_INT_POSITIVE("_Primaries", format->color_primaries);

	vsapi->propSetInt(props, "_FieldBased", 0, paReplace);
#undef COPY_INT_POSITIVE
}

static void propagate_sar(const VSAPI *vsapi, const VSMap *src_props, VSMap *dst_props, const zimg_image_format *src_format, const zimg_image_format *dst_format)
{
	int64_t sar_num = 0;
	int64_t sar_den = 0;
	int err;

	if ((sar_num = vsapi->propGetInt(src_props, "_SARNum", 0, &err)), err)
		sar_num = 0;
	if ((sar_den = vsapi->propGetInt(src_props, "_SARDen", 0, &err)), err)
		sar_den = 0;

	if (sar_num <= 0 || sar_den <= 0) {
		vsapi->propDeleteKey(dst_props, "_SARNum");
		vsapi->propDeleteKey(dst_props, "_SARDen");
	} else {
		muldivRational(&sar_num, &sar_den, dst_format->width, src_format->width);
		muldivRational(&sar_num, &sar_den, src_format->height, dst_format->height);

		vsapi->propSetInt(dst_props, "_SARNum", sar_num, paReplace);
		vsapi->propSetInt(dst_props, "_SARDen", sar_den, paReplace);
	}
}


struct vszimg_graph_data {
	zimg_filter_graph *graph;
	zimg_image_format src_format;
	zimg_image_format dst_format;
	int ref_count;
};

struct vszimg_data {
	struct vszimg_graph_data *graph_data;
	vszimg_mutex_t graph_mutex;
	vszimg_bool graph_mutex_initialized;

	VSNodeRef *node;
	VSVideoInfo vi;
	zimg_filter_graph_params params;

	zimg_matrix_coefficients_e matrix;
	zimg_transfer_characteristics_e transfer;
	zimg_color_primaries_e primaries;
	zimg_pixel_range_e range;
	zimg_chroma_location_e chromaloc;

	zimg_matrix_coefficients_e matrix_in;
	zimg_transfer_characteristics_e transfer_in;
	zimg_color_primaries_e primaries_in;
	zimg_pixel_range_e range_in;
	zimg_chroma_location_e chromaloc_in;

	vszimg_bool have_matrix;
	vszimg_bool have_transfer;
	vszimg_bool have_primaries;
	vszimg_bool have_range;
	vszimg_bool have_chromaloc;

	vszimg_bool have_matrix_in;
	vszimg_bool have_transfer_in;
	vszimg_bool have_primaries_in;
	vszimg_bool have_range_in;
	vszimg_bool have_chromaloc_in;
};

static void _vszimg_default_init(struct vszimg_data *data)
{
	memset(data, 0, sizeof(*data));

	data->graph_data = NULL;
	data->graph_mutex_initialized = VSZIMG_FALSE;
	data->node = NULL;
}

static void _vszimg_destroy(struct vszimg_data *data, const VSAPI *vsapi)
{
	if (data->graph_data)
		zimg2_filter_graph_free(data->graph_data->graph);
	if (data->graph_mutex_initialized)
		vszimg_mutex_destroy(&data->graph_mutex);

	free(data->graph_data);
	vsapi->freeNode(data->node);
}

static void _vszimg_set_src_colorspace(const struct vszimg_data *data, zimg_image_format *src_format)
{
	if (data->have_matrix_in)
		src_format->matrix_coefficients = data->matrix_in;
	if (data->have_transfer_in)
		src_format->transfer_characteristics = data->transfer_in;
	if (data->have_primaries_in)
		src_format->color_primaries = data->primaries_in;
	if (data->have_range_in)
		src_format->pixel_range = data->range_in;
	if (data->have_chromaloc_in)
		src_format->chroma_location = data->chromaloc_in;
}

static void _vszimg_set_dst_colorspace(const struct vszimg_data *data, const zimg_image_format *src_format, zimg_image_format *dst_format)
{
	/* Avoid propagating source matrix coefficients if the target is RGB. */
	if (dst_format->color_family != ZIMG_COLOR_RGB)
		dst_format->matrix_coefficients = src_format->matrix_coefficients;

	dst_format->transfer_characteristics = src_format->transfer_characteristics;
	dst_format->color_primaries = src_format->color_primaries;

	/* Avoid propagating source pixel range and chroma location if color family changes. */
	if (dst_format->color_family == src_format->color_family) {
		dst_format->pixel_range = src_format->pixel_range;
		dst_format->chroma_location = src_format->chroma_location;
	}

	if (data->have_matrix)
		dst_format->matrix_coefficients = data->matrix;
	if (data->have_transfer)
		dst_format->transfer_characteristics = data->transfer;
	if (data->have_primaries)
		dst_format->color_primaries = data->primaries;
	if (data->have_range)
		dst_format->pixel_range = data->range;
	if (data->have_chromaloc)
		dst_format->chroma_location = data->chromaloc;
}

static void _vszimg_release_graph_data_unsafe(struct vszimg_graph_data *graph_data)
{
	if (!graph_data)
		return;
	if (--graph_data->ref_count == 0) {
		zimg2_filter_graph_free(graph_data->graph);
		free(graph_data);
	}
}

static void _vszimg_release_graph_data(struct vszimg_data *data, struct vszimg_graph_data *graph_data)
{
	if (!graph_data)
		return;
	if (vszimg_mutex_lock(&data->graph_mutex))
		return;

	_vszimg_release_graph_data_unsafe(graph_data);

	vszimg_mutex_unlock(&data->graph_mutex);
}

static struct vszimg_graph_data *_vszimg_get_graph_data(struct vszimg_data *data, const zimg_image_format *src_format, const zimg_image_format *dst_format,
                                                        char *err_msg, size_t err_msg_size)
{
	struct vszimg_graph_data *graph_data = NULL;
	struct vszimg_graph_data *ret = NULL;
	vszimg_bool mutex_locked = VSZIMG_FALSE;
	vszimg_bool allocate_new;

	if (vszimg_mutex_lock(&data->graph_mutex)) {
		sprintf(err_msg, "error locking mutex");
		goto fail;
	}
	mutex_locked = VSZIMG_TRUE;

	allocate_new = !data->graph_data ||
	               (!image_format_eq(&data->graph_data->src_format, src_format) ||
	                !image_format_eq(&data->graph_data->dst_format, dst_format));

	if (allocate_new) {
		if (!(graph_data = malloc(sizeof(*graph_data)))) {
			sprintf(err_msg, "error allocating vszimg_graph_data");
			goto fail;
		}
		graph_data->graph = NULL;
		graph_data->ref_count = 1;

		if (!(graph_data->graph = zimg2_filter_graph_build(src_format, dst_format, &data->params))) {
			format_zimg_error(err_msg, err_msg_size);
			goto fail;
		}

		graph_data->src_format = *src_format;
		graph_data->dst_format = *dst_format;

		_vszimg_release_graph_data_unsafe(data->graph_data);

		++graph_data->ref_count;
		data->graph_data = graph_data;

		ret = graph_data;
		graph_data = NULL;
	} else {
		++data->graph_data->ref_count;
		ret = data->graph_data;
	}
fail:
	if (mutex_locked)
		vszimg_mutex_unlock(&data->graph_mutex);

	_vszimg_release_graph_data(data, graph_data);
	return ret;
}

static void VS_CC vszimg_init(VSMap *in, VSMap *out, void **instanceData, VSNode *node, VSCore *core, const VSAPI *vsapi)
{
	struct vszimg_data *data = *instanceData;
	vsapi->setVideoInfo(&data->vi, 1, node);
}

static const VSFrameRef * VS_CC vszimg_get_frame(int n, int activationReason, void **instanceData, void **frameData, VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi)
{
	struct vszimg_data *data = *instanceData;
	struct vszimg_graph_data *graph_data = NULL;
	const VSFrameRef *src_frame = NULL;
	VSFrameRef *dst_frame = NULL;
	VSFrameRef *ret = NULL;
	void *tmp = NULL;
	char err_msg[1024];
	int err_flag = 1;

	if (activationReason == arInitial) {
		vsapi->requestFrameFilter(n, data->node, frameCtx);
	} else if (activationReason == arAllFramesReady) {
		zimg_image_buffer_const src_buf = { ZIMG_API_VERSION };
		zimg_image_buffer dst_buf = { { ZIMG_API_VERSION } };
		zimg_image_format src_format;
		zimg_image_format dst_format;
		const VSFormat *src_vsformat;
		const VSFormat *dst_vsformat;
		const VSMap *src_props;
		VSMap *dst_props;
		size_t tmp_size;
		int p;

		zimg2_image_format_default(&src_format, ZIMG_API_VERSION);
		zimg2_image_format_default(&dst_format, ZIMG_API_VERSION);

		src_frame = vsapi->getFrameFilter(n, data->node, frameCtx);
		src_props = vsapi->getFramePropsRO(src_frame);

		src_vsformat = vsapi->getFrameFormat(src_frame);
		dst_vsformat = data->vi.format ? data->vi.format : src_vsformat;

		src_format.width = vsapi->getFrameWidth(src_frame, 0);
		src_format.height = vsapi->getFrameHeight(src_frame, 0);

		dst_format.width = data->vi.width ? (unsigned)data->vi.width : src_format.width;
		dst_format.height = data->vi.height ? (unsigned)data->vi.height : src_format.height;

		if (translate_vsformat(src_vsformat, &src_format, err_msg))
			goto fail;
		if (translate_vsformat(dst_vsformat, &dst_format, err_msg))
			goto fail;

		_vszimg_set_src_colorspace(data, &src_format);

		if (import_frame_props(vsapi, src_props, &src_format, err_msg))
			goto fail;

		_vszimg_set_dst_colorspace(data, &src_format, &dst_format);

		if (!(graph_data = _vszimg_get_graph_data(data, &src_format, &dst_format, err_msg, sizeof(err_msg))))
			goto fail;

		dst_frame = vsapi->newVideoFrame(dst_vsformat, dst_format.width, dst_format.height, src_frame, core);
		dst_props = vsapi->getFramePropsRW(dst_frame);

		for (p = 0; p < src_vsformat->numPlanes; ++p) {
			src_buf.data[p] = vsapi->getReadPtr(src_frame, p);
			src_buf.stride[p] = vsapi->getStride(src_frame, p);
			src_buf.mask[p] = -1;
		}
		for (p = 0; p < dst_vsformat->numPlanes; ++p) {
			dst_buf.m.data[p] = vsapi->getWritePtr(dst_frame, p);
			dst_buf.m.stride[p] = vsapi->getStride(dst_frame, p);
			dst_buf.m.mask[p] = -1;
		}

		if (zimg2_filter_graph_get_tmp_size(graph_data->graph, &tmp_size)) {
			format_zimg_error(err_msg, sizeof(err_msg));
			goto fail;
		}

		VS_ALIGNED_MALLOC(&tmp, tmp_size, 64);
		if (!tmp) {
			sprintf(err_msg, "error allocating temporary buffer");
			goto fail;
		}

		if (zimg2_filter_graph_process(graph_data->graph, &src_buf, &dst_buf, tmp, NULL, NULL, NULL, NULL)) {
			format_zimg_error(err_msg, sizeof(err_msg));
			goto fail;
		}

		propagate_sar(vsapi, src_props, dst_props, &src_format, &dst_format);
		export_frame_props(vsapi, &dst_format, dst_props);

		ret = dst_frame;
		dst_frame = NULL;
	}

	err_flag = 0;
fail:
	if (err_flag)
		vsapi->setFilterError(err_msg, frameCtx);

	_vszimg_release_graph_data(data, graph_data);
	vsapi->freeFrame(src_frame);
	vsapi->freeFrame(dst_frame);
	return ret;
}

static void VS_CC vszimg_free(void *instanceData, VSCore *core, const VSAPI *vsapi)
{
	struct vszimg_data *data = instanceData;

	_vszimg_destroy(data, vsapi);
	free(data);
}

static void VS_CC vszimg_create(const VSMap *in, VSMap *out, void *userData, VSCore *core, const VSAPI *vsapi)
{
	struct vszimg_data *data = NULL;

	const VSVideoInfo *node_vi;
	const VSFormat *node_fmt;
	int format_id;

	char err_msg[64];

#define FAIL_BAD_VALUE(name) \
  do { \
    sprintf(err_msg, "%s: bad value", (name)); \
    goto fail; \
  } while (0)

#define TRY_GET_ENUM(name, out, flag, table) \
  do { \
    int enum_tmp; \
    if (tryGetEnum(vsapi, in, (name), &enum_tmp, &(flag), (table), ARRAY_SIZE((table)))) \
      FAIL_BAD_VALUE(name); \
    if ((flag)) \
      (out) = enum_tmp; \
  } while (0)

#define TRY_GET_ENUM_STR(name, out, table) \
  do { \
    int enum_tmp; \
    vszimg_bool flag; \
    if (tryGetEnumStr(vsapi, in, (name), &enum_tmp, &flag, (table), ARRAY_SIZE((table)))) \
      FAIL_BAD_VALUE(name); \
    if ((flag)) \
      (out) = enum_tmp; \
  } while (0)

	if (!(data = malloc(sizeof(*data)))) {
		sprintf(err_msg, "error allocating vszimg_data");
		goto fail;
	}

	_vszimg_default_init(data);

	if (vszimg_mutex_init(&data->graph_mutex)) {
		sprintf(err_msg, "error initializing mutex");
		goto fail;
	}
	data->graph_mutex_initialized = VSZIMG_TRUE;

	data->node = vsapi->propGetNode(in, "clip", 0, NULL);
	node_vi = vsapi->getVideoInfo(data->node);
	node_fmt = node_vi->format;

	data->vi = *node_vi;

	zimg2_filter_graph_params_default(&data->params, ZIMG_API_VERSION);

	if (propGetUintDef(vsapi, in, "width", &data->vi.width, node_vi->width))
		FAIL_BAD_VALUE("width");
	if (propGetUintDef(vsapi, in, "height", &data->vi.height, node_vi->height))
		FAIL_BAD_VALUE("height");

	if (propGetSintDef(vsapi, in, "format", &format_id, pfNone))
		FAIL_BAD_VALUE("format");
	data->vi.format = (format_id == pfNone) ? node_fmt : vsapi->getFormatPreset(format_id, core);

	TRY_GET_ENUM("matrix", data->matrix, data->have_matrix, g_matrix_table);
	TRY_GET_ENUM("transfer", data->transfer, data->have_transfer, g_transfer_table);
	TRY_GET_ENUM("primaries", data->primaries, data->have_primaries, g_primaries_table);
	TRY_GET_ENUM("range", data->range, data->have_range, g_range_table);
	TRY_GET_ENUM("chromaloc", data->chromaloc, data->have_chromaloc, g_chromaloc_table);

	TRY_GET_ENUM("matrix_in", data->matrix_in, data->have_matrix_in, g_matrix_table);
	TRY_GET_ENUM("transfer_in", data->transfer_in, data->have_transfer_in, g_transfer_table);
	TRY_GET_ENUM("primaries_in", data->primaries_in, data->have_primaries_in, g_primaries_table);
	TRY_GET_ENUM("range_in", data->range_in, data->have_range_in, g_range_table);
	TRY_GET_ENUM("chromaloc_in", data->chromaloc_in, data->have_chromaloc_in, g_chromaloc_table);

	TRY_GET_ENUM_STR("resample_filter", data->params.resample_filter, g_resample_filter_table);
	data->params.filter_param_a = propGetFloatDef(vsapi, in, "filter_param_a", data->params.filter_param_a);
	data->params.filter_param_b = propGetFloatDef(vsapi, in, "filter_param_b", data->params.filter_param_b);

	TRY_GET_ENUM_STR("resample_filter_uv", data->params.resample_filter_uv, g_resample_filter_table);
	data->params.filter_param_a_uv = propGetFloatDef(vsapi, in, "filter_param_a_uv", data->params.filter_param_a_uv);
	data->params.filter_param_b_uv = propGetFloatDef(vsapi, in, "filter_param_b_uv", data->params.filter_param_b_uv);

	TRY_GET_ENUM_STR("dither_type", data->params.dither_type, g_dither_type_table);
	TRY_GET_ENUM_STR("cpu_type", data->params.cpu_type, g_cpu_type_table);

#undef FAIL_BAD_VALUE
#undef TRY_GET_ENUM
#undef TRY_GET_ENUM_STR

	/* Basic compatibility check. */
	if (isConstantFormat(node_vi) && isConstantFormat(&data->vi)) {
		zimg_image_format src_format;
		zimg_image_format dst_format;

		zimg2_image_format_default(&src_format, ZIMG_API_VERSION);
		zimg2_image_format_default(&dst_format, ZIMG_API_VERSION);

		src_format.width = node_vi->width;
		src_format.height = node_vi->height;

		dst_format.width = data->vi.width;
		dst_format.height = data->vi.height;

		if (translate_vsformat(node_vi->format, &src_format, err_msg))
			goto fail;
		if (translate_vsformat(data->vi.format, &dst_format, err_msg))
			goto fail;
	}

	vsapi->createFilter(in, out, "format", vszimg_init, vszimg_get_frame, vszimg_free, fmParallel, 0, data, core);
	return;
fail:
	vsapi->setError(out, err_msg);
	_vszimg_destroy(data, vsapi);
	free(data);
}

VS_EXTERNAL_API(void) VapourSynthPluginInit(VSConfigPlugin configFunc, VSRegisterFunction registerFunc, VSPlugin *plugin)
{
#define INT_OPT(x) #x":int:opt;"
#define FLOAT_OPT(x) #x":float:opt;"
#define DATA_OPT(x) #x":data:opt;"
#define ENUM_OPT(x) INT_OPT(x)DATA_OPT(x ## _s)
	static const char FORMAT_DEFINITION[] =
		"clip:clip;"
		INT_OPT(width)
		INT_OPT(height)
		INT_OPT(format)
		ENUM_OPT(matrix)
		ENUM_OPT(transfer)
		ENUM_OPT(primaries)
		ENUM_OPT(range)
		ENUM_OPT(chromaloc)
		ENUM_OPT(matrix_in)
		ENUM_OPT(transfer_in)
		ENUM_OPT(primaries_in)
		ENUM_OPT(range_in)
		ENUM_OPT(chromaloc_in)
		DATA_OPT(resample_filter)
		FLOAT_OPT(filter_param_a)
		FLOAT_OPT(filter_param_b)
		DATA_OPT(resample_filter_uv)
		FLOAT_OPT(filter_param_a_uv)
		FLOAT_OPT(filter_param_b_uv)
		DATA_OPT(dither_type)
		DATA_OPT(cpu_type);
#undef INT_OPT
#undef FLOAT_OPT
#undef DATA_OPT
#undef ENUM_OPT

	zimg2_get_version_info(&g_version_info[0], &g_version_info[1], &g_version_info[2]);
	g_api_version = zimg2_get_api_version();

	configFunc("the.weather.channel", "z", "batman", VAPOURSYNTH_API_VERSION, 1, plugin);

	registerFunc("Format", FORMAT_DEFINITION, vszimg_create, NULL, plugin);
}
