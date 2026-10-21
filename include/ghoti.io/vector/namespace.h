/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Vector.
 *
 * Ghoti.io Vector is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * Ghoti.io Vector is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file namespace.h
 *
 * Maps every public name of this library into its version namespace.
 *
 * Kept in one file rather than beside each declaration: a type rename has to
 * be in effect before any struct tag that uses the name, and an internal
 * header may define such a tag without including the public header that
 * declares the typedef.
 *
 * `make check-symbols` fails if an exported symbol is missing from this list.
 */

#ifndef GHOTI_IO_GVEC_NAMESPACE_H
#define GHOTI_IO_GVEC_NAMESPACE_H

#include <ghoti.io/vector/libver.h>

/// @cond HIDDEN_SYMBOLS

/* Public types, with their tags. */
#define GVEC_Allocator GHOTIIO_VECTOR(GVEC_Allocator)
#define GVEC_Codec GHOTIIO_VECTOR(GVEC_Codec)
#define GVEC_Diag_Code GHOTIIO_VECTOR(GVEC_Diag_Code)
#define GVEC_Diag_Severity GHOTIIO_VECTOR(GVEC_Diag_Severity)
#define GVEC_Diagnostic GHOTIIO_VECTOR(GVEC_Diagnostic)
#define GVEC_Diagnostics GHOTIIO_VECTOR(GVEC_Diagnostics)
#define GVEC_Doc GHOTIIO_VECTOR(GVEC_Doc)
#define GVEC_Limit_Field GHOTIIO_VECTOR(GVEC_Limit_Field)
#define GVEC_Limits GHOTIIO_VECTOR(GVEC_Limits)
#define GVEC_Load_Options GHOTIIO_VECTOR(GVEC_Load_Options)
#define GVEC_Lower_Options GHOTIIO_VECTOR(GVEC_Lower_Options)
#define GVEC_Lower_Reason GHOTIIO_VECTOR(GVEC_Lower_Reason)
#define GVEC_Probe_Result GHOTIIO_VECTOR(GVEC_Probe_Result)
#define GVEC_Result GHOTIIO_VECTOR(GVEC_Result)
#define GVEC_Stream GHOTIIO_VECTOR(GVEC_Stream)
#define GVEC_Transport GHOTIIO_VECTOR(GVEC_Transport)

/* Internal functions and objects (the src internal headers), for the tests. */
#define gvec_alloc GHOTIIO_VECTOR(gvec_alloc)
#define gvec_diag_add GHOTIIO_VECTOR(gvec_diag_add)
#define gvec_diag_set_cap GHOTIIO_VECTOR(gvec_diag_set_cap)
#define gvec_dump_text GHOTIIO_VECTOR(gvec_dump_text)
#define gvec_free GHOTIIO_VECTOR(gvec_free)
#define gvec_limits_ceiling GHOTIIO_VECTOR(gvec_limits_ceiling)
#define gvec_limits_resolve GHOTIIO_VECTOR(gvec_limits_resolve)
#define gvec_realloc GHOTIIO_VECTOR(gvec_realloc)
#define gvec_stream_consume GHOTIIO_VECTOR(gvec_stream_consume)
#define gvec_stream_create_memory_chunked_with_allocator GHOTIIO_VECTOR(gvec_stream_create_memory_chunked_with_allocator)
#define gvec_stream_create_memory_no_seek_with_allocator GHOTIIO_VECTOR(gvec_stream_create_memory_no_seek_with_allocator)
#define gvec_stream_lookahead_all GHOTIIO_VECTOR(gvec_stream_lookahead_all)
#define gvec_svg_codec GHOTIIO_VECTOR(gvec_svg_codec)
#define gvec_svg_free GHOTIIO_VECTOR(gvec_svg_free)
#define gvec_svg_load GHOTIIO_VECTOR(gvec_svg_load)
#define gvec_svg_lower_edit GHOTIIO_VECTOR(gvec_svg_lower_edit)
#define gvec_svg_lower_list GHOTIIO_VECTOR(gvec_svg_lower_list)
#define gvec_svg_probe GHOTIIO_VECTOR(gvec_svg_probe)
#define gvec_svg_xml_options GHOTIIO_VECTOR(gvec_svg_xml_options)
#define gvec_transport_decode GHOTIIO_VECTOR(gvec_transport_decode)
#define gvec_transport_free GHOTIIO_VECTOR(gvec_transport_free)
#define gvec_transport_is_gzip GHOTIIO_VECTOR(gvec_transport_is_gzip)

/* Public functions. */
#define gvec_allocator_default GHOTIIO_VECTOR(gvec_allocator_default)
#define gvec_codec_by_index GHOTIIO_VECTOR(gvec_codec_by_index)
#define gvec_codec_by_name GHOTIIO_VECTOR(gvec_codec_by_name)
#define gvec_codec_capabilities GHOTIIO_VECTOR(gvec_codec_capabilities)
#define gvec_codec_count GHOTIIO_VECTOR(gvec_codec_count)
#define gvec_codec_name GHOTIIO_VECTOR(gvec_codec_name)
#define gvec_diagnostics_clear GHOTIIO_VECTOR(gvec_diagnostics_clear)
#define gvec_diagnostics_count_code GHOTIIO_VECTOR(gvec_diagnostics_count_code)
#define gvec_diagnostics_destroy GHOTIIO_VECTOR(gvec_diagnostics_destroy)
#define gvec_diagnostics_init GHOTIIO_VECTOR(gvec_diagnostics_init)
#define gvec_doc_codec GHOTIIO_VECTOR(gvec_doc_codec)
#define gvec_doc_dump GHOTIIO_VECTOR(gvec_doc_dump)
#define gvec_doc_format GHOTIIO_VECTOR(gvec_doc_format)
#define gvec_doc_free GHOTIIO_VECTOR(gvec_doc_free)
#define gvec_doc_hash GHOTIIO_VECTOR(gvec_doc_hash)
#define gvec_doc_load GHOTIIO_VECTOR(gvec_doc_load)
#define gvec_doc_lower_edit GHOTIIO_VECTOR(gvec_doc_lower_edit)
#define gvec_doc_lower_list GHOTIIO_VECTOR(gvec_doc_lower_list)
#define gvec_doc_transport GHOTIIO_VECTOR(gvec_doc_transport)
#define gvec_limits_default GHOTIIO_VECTOR(gvec_limits_default)
#define gvec_probe GHOTIIO_VECTOR(gvec_probe)
#define gvec_result_string GHOTIIO_VECTOR(gvec_result_string)
#define gvec_stream_create_file GHOTIIO_VECTOR(gvec_stream_create_file)
#define gvec_stream_create_file_with_allocator GHOTIIO_VECTOR(gvec_stream_create_file_with_allocator)
#define gvec_stream_create_memory GHOTIIO_VECTOR(gvec_stream_create_memory)
#define gvec_stream_create_memory_output GHOTIIO_VECTOR(gvec_stream_create_memory_output)
#define gvec_stream_create_memory_output_with_allocator GHOTIIO_VECTOR(gvec_stream_create_memory_output_with_allocator)
#define gvec_stream_create_memory_with_allocator GHOTIIO_VECTOR(gvec_stream_create_memory_with_allocator)
#define gvec_stream_destroy GHOTIIO_VECTOR(gvec_stream_destroy)
#define gvec_stream_eof GHOTIIO_VECTOR(gvec_stream_eof)
#define gvec_stream_error GHOTIIO_VECTOR(gvec_stream_error)
#define gvec_stream_output_buffer GHOTIIO_VECTOR(gvec_stream_output_buffer)
#define gvec_stream_peek GHOTIIO_VECTOR(gvec_stream_peek)
#define gvec_stream_read GHOTIIO_VECTOR(gvec_stream_read)
#define gvec_stream_read_exact GHOTIIO_VECTOR(gvec_stream_read_exact)
#define gvec_stream_seek GHOTIIO_VECTOR(gvec_stream_seek)
#define gvec_stream_size GHOTIIO_VECTOR(gvec_stream_size)
#define gvec_stream_tell GHOTIIO_VECTOR(gvec_stream_tell)
#define gvec_stream_write GHOTIIO_VECTOR(gvec_stream_write)
#define gvec_version_number GHOTIIO_VECTOR(gvec_version_number)
#define gvec_version_string GHOTIIO_VECTOR(gvec_version_string)

/// @endcond

#endif // GHOTI_IO_GVEC_NAMESPACE_H
