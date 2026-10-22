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
#define GVEC_Svg_Align GHOTIIO_VECTOR(GVEC_Svg_Align)
#define GVEC_Svg_Attribute GHOTIIO_VECTOR(GVEC_Svg_Attribute)
#define GVEC_Svg_Attribute_Class GHOTIIO_VECTOR(GVEC_Svg_Attribute_Class)
#define GVEC_Svg_Data_Uri GHOTIIO_VECTOR(GVEC_Svg_Data_Uri)
#define GVEC_Svg_Document GHOTIIO_VECTOR(GVEC_Svg_Document)
#define GVEC_Svg_Element GHOTIIO_VECTOR(GVEC_Svg_Element)
#define GVEC_Svg_Grammar GHOTIIO_VECTOR(GVEC_Svg_Grammar)
#define GVEC_Svg_Kind GHOTIIO_VECTOR(GVEC_Svg_Kind)
#define GVEC_Svg_Length GHOTIIO_VECTOR(GVEC_Svg_Length)
#define GVEC_Svg_Node GHOTIIO_VECTOR(GVEC_Svg_Node)
#define GVEC_Svg_Orient GHOTIIO_VECTOR(GVEC_Svg_Orient)
#define GVEC_Svg_Orient_Mode GHOTIIO_VECTOR(GVEC_Svg_Orient_Mode)
#define GVEC_Svg_Path_Command GHOTIIO_VECTOR(GVEC_Svg_Path_Command)
#define GVEC_Svg_Path_Segment GHOTIIO_VECTOR(GVEC_Svg_Path_Segment)
#define GVEC_Svg_Preserve_Aspect_Ratio GHOTIIO_VECTOR(GVEC_Svg_Preserve_Aspect_Ratio)
#define GVEC_Svg_Ref_Kind GHOTIIO_VECTOR(GVEC_Svg_Ref_Kind)
#define GVEC_Svg_Reference GHOTIIO_VECTOR(GVEC_Svg_Reference)
#define GVEC_Svg_Segment_Kind GHOTIIO_VECTOR(GVEC_Svg_Segment_Kind)
#define GVEC_Svg_Unit GHOTIIO_VECTOR(GVEC_Svg_Unit)
#define GVEC_Svg_Value_Kind GHOTIIO_VECTOR(GVEC_Svg_Value_Kind)
#define GVEC_Svg_View_Box GHOTIIO_VECTOR(GVEC_Svg_View_Box)
#define GVEC_Transport GHOTIIO_VECTOR(GVEC_Transport)

/* Internal functions and objects (the src internal headers), for the tests. */
#define gvec_alloc GHOTIIO_VECTOR(gvec_alloc)
#define gvec_arena_alloc GHOTIIO_VECTOR(gvec_arena_alloc)
#define gvec_arena_array GHOTIIO_VECTOR(gvec_arena_array)
#define gvec_arena_destroy GHOTIIO_VECTOR(gvec_arena_destroy)
#define gvec_arena_init GHOTIIO_VECTOR(gvec_arena_init)
#define gvec_arena_strndup GHOTIIO_VECTOR(gvec_arena_strndup)
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
#define gvec_svg_angle_degrees GHOTIIO_VECTOR(gvec_svg_angle_degrees)
#define gvec_svg_build GHOTIIO_VECTOR(gvec_svg_build)
#define gvec_svg_codec GHOTIIO_VECTOR(gvec_svg_codec)
#define gvec_svg_conditional GHOTIIO_VECTOR(gvec_svg_conditional)
#define gvec_svg_document_free GHOTIIO_VECTOR(gvec_svg_document_free)
#define gvec_svg_document_of GHOTIIO_VECTOR(gvec_svg_document_of)
#define gvec_svg_dump_document GHOTIIO_VECTOR(gvec_svg_dump_document)
#define gvec_svg_free GHOTIIO_VECTOR(gvec_svg_free)
#define gvec_svg_grammar_for GHOTIIO_VECTOR(gvec_svg_grammar_for)
#define gvec_svg_grammar_name GHOTIIO_VECTOR(gvec_svg_grammar_name)
#define gvec_svg_hash64 GHOTIIO_VECTOR(gvec_svg_hash64)
#define gvec_svg_is_legacy GHOTIIO_VECTOR(gvec_svg_is_legacy)
#define gvec_svg_is_presentation GHOTIIO_VECTOR(gvec_svg_is_presentation)
#define gvec_svg_is_wsp GHOTIIO_VECTOR(gvec_svg_is_wsp)
#define gvec_svg_key_for GHOTIIO_VECTOR(gvec_svg_key_for)
#define gvec_svg_key_rehash GHOTIIO_VECTOR(gvec_svg_key_rehash)
#define gvec_svg_kind_find GHOTIIO_VECTOR(gvec_svg_kind_find)
#define gvec_svg_load GHOTIIO_VECTOR(gvec_svg_load)
#define gvec_svg_lower_edit GHOTIIO_VECTOR(gvec_svg_lower_edit)
#define gvec_svg_lower_list GHOTIIO_VECTOR(gvec_svg_lower_list)
#define gvec_svg_parse_angle GHOTIIO_VECTOR(gvec_svg_parse_angle)
#define gvec_svg_parse_extension_list GHOTIIO_VECTOR(gvec_svg_parse_extension_list)
#define gvec_svg_parse_failed GHOTIIO_VECTOR(gvec_svg_parse_failed)
#define gvec_svg_parse_integer GHOTIIO_VECTOR(gvec_svg_parse_integer)
#define gvec_svg_parse_keyword GHOTIIO_VECTOR(gvec_svg_parse_keyword)
#define gvec_svg_parse_language_list GHOTIIO_VECTOR(gvec_svg_parse_language_list)
#define gvec_svg_parse_length GHOTIIO_VECTOR(gvec_svg_parse_length)
#define gvec_svg_parse_length_list GHOTIIO_VECTOR(gvec_svg_parse_length_list)
#define gvec_svg_parse_number GHOTIIO_VECTOR(gvec_svg_parse_number)
#define gvec_svg_parse_number_list GHOTIIO_VECTOR(gvec_svg_parse_number_list)
#define gvec_svg_parse_offset GHOTIIO_VECTOR(gvec_svg_parse_offset)
#define gvec_svg_parse_orient GHOTIIO_VECTOR(gvec_svg_parse_orient)
#define gvec_svg_parse_par GHOTIIO_VECTOR(gvec_svg_parse_par)
#define gvec_svg_parse_path GHOTIIO_VECTOR(gvec_svg_parse_path)
#define gvec_svg_parse_points GHOTIIO_VECTOR(gvec_svg_parse_points)
#define gvec_svg_parse_ref_coord GHOTIIO_VECTOR(gvec_svg_parse_ref_coord)
#define gvec_svg_parse_reference GHOTIIO_VECTOR(gvec_svg_parse_reference)
#define gvec_svg_parse_view_box GHOTIIO_VECTOR(gvec_svg_parse_view_box)
#define gvec_svg_probe GHOTIIO_VECTOR(gvec_svg_probe)
#define gvec_svg_resolve GHOTIIO_VECTOR(gvec_svg_resolve)
#define gvec_svg_scan_number GHOTIIO_VECTOR(gvec_svg_scan_number)
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
#define gvec_svg_attribute_class GHOTIIO_VECTOR(gvec_svg_attribute_class)
#define gvec_svg_attribute_column GHOTIIO_VECTOR(gvec_svg_attribute_column)
#define gvec_svg_attribute_error_offset GHOTIIO_VECTOR(gvec_svg_attribute_error_offset)
#define gvec_svg_attribute_error_reason GHOTIIO_VECTOR(gvec_svg_attribute_error_reason)
#define gvec_svg_attribute_grammar GHOTIIO_VECTOR(gvec_svg_attribute_grammar)
#define gvec_svg_attribute_integer GHOTIIO_VECTOR(gvec_svg_attribute_integer)
#define gvec_svg_attribute_is_partial GHOTIIO_VECTOR(gvec_svg_attribute_is_partial)
#define gvec_svg_attribute_is_presentation GHOTIIO_VECTOR(gvec_svg_attribute_is_presentation)
#define gvec_svg_attribute_keyword GHOTIIO_VECTOR(gvec_svg_attribute_keyword)
#define gvec_svg_attribute_length GHOTIIO_VECTOR(gvec_svg_attribute_length)
#define gvec_svg_attribute_lengths GHOTIIO_VECTOR(gvec_svg_attribute_lengths)
#define gvec_svg_attribute_line GHOTIIO_VECTOR(gvec_svg_attribute_line)
#define gvec_svg_attribute_name GHOTIIO_VECTOR(gvec_svg_attribute_name)
#define gvec_svg_attribute_namespace GHOTIIO_VECTOR(gvec_svg_attribute_namespace)
#define gvec_svg_attribute_number GHOTIIO_VECTOR(gvec_svg_attribute_number)
#define gvec_svg_attribute_numbers GHOTIIO_VECTOR(gvec_svg_attribute_numbers)
#define gvec_svg_attribute_offset GHOTIIO_VECTOR(gvec_svg_attribute_offset)
#define gvec_svg_attribute_orient GHOTIIO_VECTOR(gvec_svg_attribute_orient)
#define gvec_svg_attribute_path GHOTIIO_VECTOR(gvec_svg_attribute_path)
#define gvec_svg_attribute_preserve_aspect_ratio GHOTIIO_VECTOR(gvec_svg_attribute_preserve_aspect_ratio)
#define gvec_svg_attribute_raw GHOTIIO_VECTOR(gvec_svg_attribute_raw)
#define gvec_svg_attribute_reference GHOTIIO_VECTOR(gvec_svg_attribute_reference)
#define gvec_svg_attribute_string GHOTIIO_VECTOR(gvec_svg_attribute_string)
#define gvec_svg_attribute_value_kind GHOTIIO_VECTOR(gvec_svg_attribute_value_kind)
#define gvec_svg_attribute_view_box GHOTIIO_VECTOR(gvec_svg_attribute_view_box)
#define gvec_svg_data_uri_decode GHOTIIO_VECTOR(gvec_svg_data_uri_decode)
#define gvec_svg_document GHOTIIO_VECTOR(gvec_svg_document)
#define gvec_svg_element_attribute_at GHOTIIO_VECTOR(gvec_svg_element_attribute_at)
#define gvec_svg_element_attribute_count GHOTIIO_VECTOR(gvec_svg_element_attribute_count)
#define gvec_svg_element_column GHOTIIO_VECTOR(gvec_svg_element_column)
#define gvec_svg_element_conditional_false GHOTIIO_VECTOR(gvec_svg_element_conditional_false)
#define gvec_svg_element_count GHOTIIO_VECTOR(gvec_svg_element_count)
#define gvec_svg_element_expansion GHOTIIO_VECTOR(gvec_svg_element_expansion)
#define gvec_svg_element_expansion_limited GHOTIIO_VECTOR(gvec_svg_element_expansion_limited)
#define gvec_svg_element_find_attribute GHOTIIO_VECTOR(gvec_svg_element_find_attribute)
#define gvec_svg_element_first_child GHOTIIO_VECTOR(gvec_svg_element_first_child)
#define gvec_svg_element_first_node GHOTIIO_VECTOR(gvec_svg_element_first_node)
#define gvec_svg_element_href_target GHOTIIO_VECTOR(gvec_svg_element_href_target)
#define gvec_svg_element_id GHOTIIO_VECTOR(gvec_svg_element_id)
#define gvec_svg_element_key GHOTIIO_VECTOR(gvec_svg_element_key)
#define gvec_svg_element_kind GHOTIIO_VECTOR(gvec_svg_element_kind)
#define gvec_svg_element_line GHOTIIO_VECTOR(gvec_svg_element_line)
#define gvec_svg_element_name GHOTIIO_VECTOR(gvec_svg_element_name)
#define gvec_svg_element_namespace GHOTIIO_VECTOR(gvec_svg_element_namespace)
#define gvec_svg_element_next_sibling GHOTIIO_VECTOR(gvec_svg_element_next_sibling)
#define gvec_svg_element_offset GHOTIIO_VECTOR(gvec_svg_element_offset)
#define gvec_svg_element_on_cycle GHOTIIO_VECTOR(gvec_svg_element_on_cycle)
#define gvec_svg_element_parent GHOTIIO_VECTOR(gvec_svg_element_parent)
#define gvec_svg_element_switch_selected GHOTIIO_VECTOR(gvec_svg_element_switch_selected)
#define gvec_svg_find_by_id GHOTIIO_VECTOR(gvec_svg_find_by_id)
#define gvec_svg_kind_name GHOTIIO_VECTOR(gvec_svg_kind_name)
#define gvec_svg_node_element GHOTIIO_VECTOR(gvec_svg_node_element)
#define gvec_svg_node_next GHOTIIO_VECTOR(gvec_svg_node_next)
#define gvec_svg_node_text GHOTIIO_VECTOR(gvec_svg_node_text)
#define gvec_svg_root GHOTIIO_VECTOR(gvec_svg_root)
#define gvec_version_number GHOTIIO_VECTOR(gvec_version_number)
#define gvec_version_string GHOTIIO_VECTOR(gvec_version_string)

/// @endcond

#endif // GHOTI_IO_GVEC_NAMESPACE_H
