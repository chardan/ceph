// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2012 Inktank Storage, Inc.
 * Copyright (C) 2014 Red Hat <contact@redhat.com>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software 
 * Foundation.  See file COPYING.
 */
#ifndef CEPH_UTIL_H
#define CEPH_UTIL_H

#include "common/Formatter.h"
#include "include/types.h"

int64_t unit_to_bytesize(string val, ostream *pss);

struct ceph_data_stats
{
  uint64_t byte_total;
  uint64_t byte_used;
  uint64_t byte_avail;
  int avail_percent;

  ceph_data_stats() :
    byte_total(0),
    byte_used(0),
    byte_avail(0),
    avail_percent(0)
  { }

  void dump(Formatter *f) const {
    assert(f != NULL);
    f->dump_int("total", byte_total);
    f->dump_int("used", byte_used);
    f->dump_int("avail", byte_avail);
    f->dump_int("avail_percent", avail_percent);
  }

  void encode(bufferlist &bl) const {
    ENCODE_START(1, 1, bl);
    ::encode(byte_total, bl);
    ::encode(byte_used, bl);
    ::encode(byte_avail, bl);
    ::encode(avail_percent, bl);
    ENCODE_FINISH(bl);
  }

  void decode(bufferlist::iterator &p) {
    DECODE_START(1, p);
    ::decode(byte_total, p);
    ::decode(byte_used, p);
    ::decode(byte_avail, p);
    ::decode(avail_percent, p);
    DECODE_FINISH(p);
  }

  static void generate_test_instances(list<ceph_data_stats*>& ls) {
    ls.push_back(new ceph_data_stats);
    ls.push_back(new ceph_data_stats);
    ls.back()->byte_total = 1024*1024;
    ls.back()->byte_used = 512*1024;
    ls.back()->byte_avail = 512*1024;
    ls.back()->avail_percent = 50;
  }
};
typedef struct ceph_data_stats ceph_data_stats_t;

int get_fs_stats(ceph_data_stats_t &stats, const char *path);

/// collect info from @p uname(2), @p /proc/meminfo and @p /proc/cpuinfo
void collect_sys_info(map<string, string> *m, CephContext *cct);

/// dump service ids grouped by their host to the specified formatter
/// @param f formatter for the output
/// @param services a map from hostname to a list of service id hosted by this host
/// @param type the service type of given @p services, for example @p osd or @p mon.
void dump_services(Formatter* f, const map<string, list<int> >& services, const char* type);

namespace librados { namespace cutil { 
inline namespace version_1_0 {

// C++17 flavor std::size():
// Adapted from: https://isocpp.org/files/papers/n4280.pdf
namespace detail {

template <typename C>
constexpr auto size(const C& c) noexcept -> decltype(c.size()) 
{
 return c.size();
}

template <typename T, std::size_t N>
constexpr std::size_t size(const T(&)[N]) noexcept
{
 return N;
}

} // namespace detail

/* A mini-serialization utility that converts sequences into NULL-delimited strings, a 
format several librados facilities use. Note that it isn't appropriate for binary data
(you'd need something like uuencoding) or situations where you need to get types back
(there's no type tagging). It's also not likely terribly "fast", though it should be
pretty resilient; it's meant to centralize the ad-hoc implementations we have now. */
template <typename SeqT>
inline size_t encoded_size(const SeqT& xs) noexcept;

template <typename FwdIBegin, typename FwdIEnd>
inline size_t encoded_size(FwdIBegin b, FwdIEnd e) noexcept;

template <typename FwdIBegin, typename FwdIEnd>
inline size_t flatten_to_cstring(FwdIBegin b, const FwdIEnd e, const size_t out_size, char *out_buffer);

template <typename SeqT>
inline size_t flatten_to_cstring(const SeqT& xs, const size_t out_size, char *out_buffer);

inline constexpr size_t encoded_size(const char c) noexcept
{
 return 1;
}

template <typename FwdIBegin, typename FwdIEnd>
inline size_t encoded_size(FwdIBegin b, const FwdIEnd e) noexcept
{
 using value_t = typename std::iterator_traits<FwdIBegin>::value_type;

 return std::accumulate(b, e, 1,
                        [](const size_t len, const value_t& x) { return len + librados::cutil::encoded_size(x); }); 
}

template <typename SeqT>
inline size_t encoded_size(const SeqT& xs) noexcept
{
 return encoded_size(std::begin(xs), std::end(xs));
}

// This is specific behavior for C strings. Remember that this is not a general encoder:
template <std::size_t N>
inline std::size_t encoded_size(const char(&)[N]) noexcept 
{
 return N;
}

template <typename T, std::size_t N>
inline std::size_t encoded_size(const T(&)[N]) noexcept
{
 return 1 + N;
}

// Note that single characters are /not/ NULL-terminated in the output buffer (just like C):
inline size_t flatten_to_cstring(const char c, const size_t out_size, char *out_buffer)
{
 if(1 > out_size) 
  throw std::out_of_range("librados::cutil::flatten_to_cstring(), out_size");

 return out_buffer[0] = c, 1;
}

inline size_t flatten_to_cstring(const char *s, const size_t out_size, char *out_buffer)
{
 return flatten_to_cstring(s, strlen(s) + s, out_size, out_buffer);
}

template <typename SeqT>
inline size_t flatten_to_cstring(const SeqT& xs, const size_t out_size, char *out_buffer) 
{
 if(out_size < librados::cutil::encoded_size(xs))
  throw std::out_of_range("librados::cutil::flatten_to_cstring(), out_size");

 return flatten_to_cstring(std::begin(xs), std::end(xs), out_size, out_buffer);
}

template <typename FwdIBegin, typename FwdIEnd>
inline size_t flatten_to_cstring(FwdIBegin b, const FwdIEnd e, const size_t out_size, char *out_buffer)
{
 size_t bytes_written = 0;

 using value_t = typename std::iterator_traits<FwdIBegin>::value_type;

 std::for_each(b, e, [&](const value_t& x) {
     bytes_written += flatten_to_cstring(x, out_size - bytes_written, out_buffer + bytes_written);
 });

 // We need room for the terminating NULL:
 if(out_size < bytes_written)
  throw std::out_of_range("librados::cutil::flatten_to_cstring(), out_size");

 out_buffer[bytes_written] = 0;

 return 1 + bytes_written;
}

} // inline namespace version_1_0
}} // namespace librados::cutil


#endif /* CEPH_UTIL_H */
