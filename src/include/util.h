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

#include <iterator>

#include "common/Formatter.h"
#include "include/types.h"

int64_t unit_to_bytesize(string val, ostream *pss);

std::string bytes2str(uint64_t count);

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
WRITE_CLASS_ENCODER(ceph_data_stats)

int get_fs_stats(ceph_data_stats_t &stats, const char *path);

/// collect info from @p uname(2), @p /proc/meminfo and @p /proc/cpuinfo
void collect_sys_info(map<string, string> *m, CephContext *cct);

/// dump service ids grouped by their host to the specified formatter
/// @param f formatter for the output
/// @param services a map from hostname to a list of service id hosted by this host
/// @param type the service type of given @p services, for example @p osd or @p mon.
void dump_services(Formatter* f, const map<string, list<int> >& services, const char* type);

string cleanbin(bufferlist &bl, bool &b64);
string cleanbin(string &str);

namespace ceph {
namespace util {

template <typename SeqT0, typename SeqT1>
void splice(SeqT0& dest, typename SeqT0::const_iterator pos, 
            SeqT1& src)
{
 dest.insert(pos,
             std::make_move_iterator(std::begin(src)), 
             std::make_move_iterator(std::end(src)));
}

template <typename SeqT0, typename SeqT1>
void splice(SeqT0& dest, typename SeqT0::const_iterator pos, 
            SeqT1&& src)
{
 splice(dest, pos, src);
}

template <typename SeqT0, typename SeqT1>
void splice(SeqT0& dest, typename SeqT0::const_iterator pos, 
            SeqT1& src, typename SeqT1::iterator src_pos)
{
 dest.insert(pos,
             std::make_move_iterator(src_pos),
             std::make_move_iterator(std::end(src)));
             
}

template <typename SeqT0, typename SeqT1>
void splice(SeqT0& dest, typename SeqT0::const_iterator pos,
            SeqT1&& src, typename SeqT1::size_type src_offset)
{
 auto i = src.begin();

 std::advance(i, src_offset);

 splice(dest, pos, src, i);
}

template <typename SeqT0, typename SeqT1>
void splice(SeqT0& dest, typename SeqT0::const_iterator pos, 
            SeqT1& src, typename SeqT1::iterator src_begin, typename SeqT1::iterator src_end)
{
 dest.insert(pos,
             std::make_move_iterator(src_begin),
             std::make_move_iterator(src_end));
}

template <typename SeqT0, typename SeqT1>
void splice(SeqT0& dest, typename SeqT0::const_iterator pos, 
            SeqT1&& src, typename SeqT1::size_type src_begin_offset, typename SeqT1::size_type src_end_offset)
{
 auto b = std::begin(src);
 std::advance(b, src_begin_offset);

 auto e = std::begin(src);
 std::advance(e, src_end_offset);

 splice(dest, pos, src, b, e);
}

}} // namespace ceph::util

#endif /* CEPH_UTIL_H */
