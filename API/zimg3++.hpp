#ifndef ZIMG3PLUSPLUS_HPP_
#define ZIMG3PLUSPLUS_HPP_

#include "zimg3.h"

namespace zimgxx {;

struct zerror {
	int code;
	char msg[1024];

	zerror()
	{
		code = zimg_get_last_error(msg, sizeof(msg));
	}
};

struct zimage_buffer_const : zimg_image_buffer_const {
	zimage_buffer_const()
	{
		version = ZIMG_API_VERSION;
	}
};

struct zimage_buffer {
	zimg_image_buffer _;

	zimage_buffer()
	{
		_.m.version = ZIMG_API_VERSION;
	}

	const zimg_image_buffer_const &as_const() const
	{
		return _.c;
	}
};

struct zimage_format : zimg_image_format {
	zimage_format()
	{
		zimg2_image_format_default(this, ZIMG_API_VERSION);
	}
};

struct zfilter_graph_params : zimg_filter_graph_params {
	zfilter_graph_params()
	{
		zimg2_filter_graph_params_default(this, ZIMG_API_VERSION);
	}
};

class FilterGraph {
private:
	zimg_filter_graph *m_graph;

	FilterGraph(const FilterGraph &);

	FilterGraph &operator=(const FilterGraph &);

	void check(int x) const
	{
		if (x)
			throw zerror();
	}
public:
	explicit FilterGraph(zimg_filter_graph *graph) : m_graph(graph)
	{
	}

	~FilterGraph()
	{
		zimg2_filter_graph_free(m_graph);
	}

	size_t get_tmp_size() const
	{
		size_t ret;
		check(zimg2_filter_graph_get_tmp_size(m_graph, &ret));
		return ret;
	}

	unsigned get_input_buffering() const
	{
		unsigned ret;
		check(zimg2_filter_graph_get_input_buffering(m_graph, &ret));
		return ret;
	}

	unsigned get_output_buffering() const
	{
		unsigned ret;
		check(zimg2_filter_graph_get_output_buffering(m_graph, &ret));
		return ret;
	}

	void process(const zimg_image_buffer_const *src, const zimg_image_buffer *dst, void *tmp,
	             zimg_filter_graph_callback unpack_cb = 0, void *unpack_user = 0,
	             zimg_filter_graph_callback pack_cb = 0, void *pack_user = 0) const
	{
		check(zimg2_filter_graph_process(m_graph, src, dst, tmp, unpack_cb, unpack_user, pack_cb, pack_user));
	}

	static zimg_filter_graph *build(const zimg_image_format *src_format, const zimg_image_format *dst_format, const zimg_filter_graph_params *params = 0)
	{
		zimg_filter_graph *graph;

		if (!(graph = zimg2_filter_graph_build(src_format, dst_format, params)))
			throw zerror();

		return graph;
	}
};

} // namespace zimgxx

#endif // ZIMG3PLUSPLUS_HPP_
