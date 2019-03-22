////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2016 by EMC Corporation, All Rights Reserved
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is EMC Corporation
///
/// @author Andrey Abramov
/// @author Vasiliy Nabatchikov
////////////////////////////////////////////////////////////////////////////////

#include "shared.hpp"
#include "field_data.hpp"
#include "field_meta.hpp"
#include "sorted_column.hpp"

#include "formats/formats.hpp"

#include "store/directory.hpp"
#include "store/store_utils.hpp"

#include "analysis/analyzer.hpp"
#include "analysis/token_attributes.hpp"
#include "analysis/token_streams.hpp"

#include "utils/bit_utils.hpp"
#include "utils/io_utils.hpp"
#include "utils/log.hpp"
#include "utils/map_utils.hpp"
#include "utils/memory.hpp"
#include "utils/object_pool.hpp"
#include "utils/timer_utils.hpp"
#include "utils/type_limits.hpp"
#include "utils/bytes_utils.hpp"

#include <set>
#include <algorithm>
#include <cassert>

NS_LOCAL

using namespace irs;

class payload : public irs::payload {
 public:
  byte_type* data() {
    return &(value_[0]);
  }

  void resize(size_t size) {
    value_.resize(size);
    value = value_;
  }

 private:
  bstring value_;
};

const byte_block_pool EMPTY_POOL;

class pos_iterator final: public irs::position {
 public:
  pos_iterator()
    : irs::position(2), // offset + payload
      prox_in_(EMPTY_POOL.begin(), 0) {
  }

  virtual void clear() override {
    pos_ = 0;
    val_ = 0; // FIXME TODO change to invalid when invalid() == 0
    offs_.clear();
    pay_.clear();
  }

  void reset(
      const field_data& field,
      const frequency& freq,
      const byte_block_pool::sliced_reader& prox
  ) {
    auto& features = field.meta().features;

    attrs_.clear();
    clear();
    has_offs_ = false;
    field_ = &field;
    freq_ = &freq;
    prox_in_ = prox;

    if (features.check<offset>()) {
      attrs_.emplace(offs_);
      has_offs_ = true;
    }

    if (features.check<payload>()) {
      attrs_.emplace(pay_);
    }
  }

  virtual uint32_t value() const override {
    return val_;
  }

  virtual bool next() override {
    assert(freq_);

    if (pos_ == freq_->value) {
      val_ = irs::type_limits<irs::type_t::pos_t>::eof();

      return false;
    }

    uint32_t pos;

    if (shift_unpack_32(irs::vread<uint32_t>(prox_in_), pos)) {
      const size_t size = irs::vread<size_t>(prox_in_);
      pay_.resize(size);
      prox_in_.read(pay_.data(), size);
    }

    val_ += pos;

    if (has_offs_) {
      offs_.start += irs::vread<uint32_t>(prox_in_);
      offs_.end = offs_.start + irs::vread<uint32_t>(prox_in_);
    }

    ++pos_;

    return true;
  }

 private:
  byte_block_pool::sliced_reader prox_in_;
  const frequency* freq_{}; // number of terms position in a document
  uint64_t pos_{}; // current position
  payload pay_{};
  offset offs_{};
  const field_data* field_{};
  uint32_t val_{};
  bool has_offs_{false}; // FIXME find a better way to handle presence of offsets
};

NS_END

NS_ROOT

bool memcmp_less(
    const byte_type* lhs, size_t lhs_size,
    const byte_type* rhs, size_t rhs_size
) NOEXCEPT {
  assert(lhs && rhs);

  const size_t size = std::min(lhs_size, rhs_size);
  const auto res = ::memcmp(lhs, rhs, size);

  if (0 == res) {
    return lhs_size < rhs_size;
  }

  return res < 0;
}

NS_BEGIN(detail)

using irs::field_data;
using irs::bytes_ref;

class doc_iterator : public irs::doc_iterator {
 public:
  doc_iterator()
    : attrs_(3), // document + frequency + position
      freq_in_(EMPTY_POOL.begin(), 0) {
  }

  virtual const attribute_view& attributes() const NOEXCEPT override {
    return attrs_;
  }

  void reset(
      const field_data& field, const irs::posting& posting,
      const byte_block_pool::sliced_reader& freq,
      const byte_block_pool::sliced_reader& prox) {
    attrs_.clear();
    attrs_.emplace(doc_);
    doc_.value = 0;
    freq_in_ = freq;
    posting_ = &posting;
    field_ = &field;

    const auto& features = field_->meta().features;

    if (true == (has_freq_ = features.check<frequency>())) {
      attrs_.emplace(freq_);
      freq_.value = 0;

      if (features.check<position>()) {
        pos_.reset(field, freq_, prox);
        attrs_.emplace(pos_);
      }
    }

  }

  virtual doc_id_t seek(doc_id_t doc) override {
    irs::seek(*this, doc);
    return value();
  }

  virtual doc_id_t value() const override {
    return doc_.value;
  }

  virtual bool next() override {
    if (freq_in_.eof()) {
      if (!doc_limits::valid(posting_->doc_code)) {
        return false;
      }

      doc_.value = posting_->doc;

      if (field_->meta().features.check<frequency>()) {
        freq_.value = posting_->freq; 
      }

      const_cast<posting*>(posting_)->doc_code = doc_limits::invalid();
    } else {
      //if (freq_) {
      if (has_freq_) {
        uint64_t delta;

        if (shift_unpack_64(irs::vread<uint64_t>(freq_in_), delta)) {
          freq_.value = 1U;
        } else {
          freq_.value = irs::vread<uint32_t>(freq_in_);
        }

        assert(delta < doc_limits::eof());
        doc_.value += doc_id_t(delta);
      } else {
        doc_.value += irs::vread<uint32_t>(freq_in_);
      }

      assert(doc_.value != posting_->doc);
    }

    pos_.clear();

    return true;
  }

 private:
  document doc_;
  frequency freq_;
  pos_iterator pos_;
  attribute_view attrs_;
  byte_block_pool::sliced_reader freq_in_;
  const posting* posting_;
  const field_data* field_;
  bool has_freq_{false}; // FIXME remove
};

class sorting_doc_iterator : public irs::doc_iterator {
 public:
  sorting_doc_iterator()
    : attrs_(3) { // document + frequency + position
    attrs_.emplace(doc_);
    attrs_.emplace(freq_);
  }

  void reset(doc_iterator& it, const doc_map& docmap) {
    const uint32_t no_frequency = 0;
    const uint32_t* freq = &no_frequency;

    const auto freq_attr = it.attributes().get<frequency>();
    if (freq_attr) {
      freq = &freq_attr->value;
    }

    docs_.clear();
    // FIXME docs_.reserve(cost)
    while (it.next()) {
      const auto new_doc = docmap.get<doc_map::NEW>(it.value() - doc_limits::min()) + doc_limits::min();

      if (doc_limits::eof(new_doc)) {
        // skip invalid documents
        continue;
      }

      docs_.emplace_back(new_doc, *freq);
    }

    // FIXME check if docs are already sorted
    std::sort(docs_.begin(), docs_.end());

    doc_.value = irs::doc_limits::invalid();
    freq_.value = 0;
    i_ = 0;
  }

  virtual const attribute_view& attributes() const NOEXCEPT override {
    return attrs_;
  }

  virtual doc_id_t seek(doc_id_t doc) NOEXCEPT override {
    irs::seek(*this, doc);
    return value();
  }

  virtual doc_id_t value() const NOEXCEPT override {
    return doc_.value;
  }

  virtual bool next() NOEXCEPT override {
    if (i_ >= docs_.size()) {
      return false;
    }

    const auto& entry = docs_[i_++];
    doc_.value = entry.doc;
    freq_.value = entry.freq;

    return true;
  }

 private:
  struct doc_entry {
    doc_entry(doc_id_t doc, uint32_t freq) NOEXCEPT
      : doc(doc), freq(freq) {
    }

    bool operator<(const doc_entry& rhs) const NOEXCEPT {
      return doc < rhs.doc;
    }

    doc_id_t doc;
    uint32_t freq;
    size_t pos_start;
  };

  size_t i_{ };
  std::vector<doc_entry> docs_;
  document doc_;
  frequency freq_;
  pos_iterator pos_;
  attribute_view attrs_;
}; // sorting_doc_iterator

class term_iterator : public irs::term_iterator {
 public:
  void reset(const field_data& field, const doc_map* docmap, const bytes_ref*& min, const bytes_ref*& max) {
    // refill postings
    postings_.clear();
    postings_.insert(field.terms_.begin(), field.terms_.end());

    max = min = &irs::bytes_ref::NIL;
    if (!postings_.empty()) {
      min = &(postings_.begin()->first);
      max = &((--postings_.end())->first);
    }

    field_ = &field;
    doc_map_ = docmap;

    // reset state
    itr_ = postings_.begin();
    itr_increment_ = false;
    term_ = irs::bytes_ref::NIL;
  }

  virtual const bytes_ref& value() const override {
    return term_;
  }

  virtual const attribute_view& attributes() const NOEXCEPT override {
    return attribute_view::empty_instance();
  }

  virtual void read() override {
    // Does nothing now
  }

  virtual irs::doc_iterator::ptr postings(const flags& /*features*/) const override {
    REGISTER_TIMER_DETAILED();
    assert(itr_ != postings_.end());

    const irs::posting& posting = itr_->second;

    // where the term's data starts
    auto ptr = field_->int_writer_->parent().seek(posting.int_start);
    const auto freq_end = *ptr; ++ptr;
    const auto prox_end = *ptr; ++ptr;
    const auto freq_begin = *ptr; ++ptr;
    const auto prox_begin = *ptr;

    auto& pool = field_->byte_writer_->parent();

    const byte_block_pool::sliced_reader freq(pool.seek(freq_begin), freq_end); // term's frequencies
    const byte_block_pool::sliced_reader prox(pool.seek(prox_begin), prox_end); // term's proximity // TODO: create on demand!!!

    doc_itr_.reset(*field_, posting, freq, prox);

    if (doc_map_) {
      sorting_doc_itr_.reset(doc_itr_, *doc_map_);
      return doc_iterator::ptr(doc_iterator::ptr(), &sorting_doc_itr_); // aliasing ctor
    }

    return doc_iterator::ptr(doc_iterator::ptr(), &doc_itr_); // aliasing ctor
  }

  virtual bool next() override {   
    if (itr_increment_) {
      ++itr_;
    }

    if (itr_ == postings_.end()) {
      itr_increment_ = false;
      term_ = irs::bytes_ref::NIL;

      return false;
    }

    itr_increment_ = true;
    term_ = itr_->first;

    return true;
  }

  const field_meta& meta() const NOEXCEPT {
    return field_->meta();
  }

 private:
  struct less_t {
    bool operator()(const bytes_ref& lhs, const bytes_ref& rhs) const NOEXCEPT {
      return memcmp_less(lhs, rhs);
    }
  };

  typedef std::map<
    bytes_ref,
    std::reference_wrapper<const posting>,
    less_t
  > map_t;

  map_t postings_;
  map_t::iterator itr_{ postings_.end() };
  irs::bytes_ref term_;
  const field_data* field_;
  const doc_map* doc_map_;
  mutable detail::doc_iterator doc_itr_;
  mutable detail::sorting_doc_iterator sorting_doc_itr_;
  bool itr_increment_{ false };
}; // term_iterator

class term_reader final : public irs::basic_term_reader, util::noncopyable {
 public:
  void reset(const field_data& field, const doc_map* docmap) {
    it_.reset(field, docmap, min_, max_);
  }

  virtual const irs::bytes_ref& (min)() const NOEXCEPT override {
    return *min_;
  }

  virtual const irs::bytes_ref& (max)() const NOEXCEPT override {
    return *max_;
  }

  virtual const irs::field_meta& meta() const NOEXCEPT override {
    return it_.meta();
  }

  virtual irs::term_iterator::ptr iterator() const NOEXCEPT override {
    return memory::make_managed<irs::term_iterator, false>(&it_);
  }

  virtual const attribute_view& attributes() const NOEXCEPT override {
    return attribute_view::empty_instance();
  }

 private:
  mutable detail::term_iterator it_;
  const irs::bytes_ref* min_{ &irs::bytes_ref::NIL };
  const irs::bytes_ref* max_{ &irs::bytes_ref::NIL };
}; // term_reader

NS_END // detail

/* -------------------------------------------------------------------
 * field_data
 * ------------------------------------------------------------------*/

void field_data::write_offset(posting& p, size_t& where, const offset& offs ) {
  const uint32_t start_offset = offs_ + offs.start;
  const uint32_t end_offset = offs_ + offs.end;

  assert(start_offset >= p.offs);

  byte_block_pool::sliced_inserter out(byte_writer_, where);

  irs::vwrite<uint32_t>(out, start_offset - p.offs);
  irs::vwrite<uint32_t>(out, end_offset - start_offset);

  where = out.pool_offset();
  p.offs = start_offset;
}

void field_data::write_prox(
    posting& p,
    size_t& where,
    uint32_t prox,
    const payload* pay
) {
  byte_block_pool::sliced_inserter out(byte_writer_, where);

  if (!pay || pay->value.empty()) {
    irs::vwrite<uint32_t>(out, shift_pack_32(prox, false));
  } else {
    irs::vwrite<uint32_t>(out, shift_pack_32(prox, true));
    irs::vwrite<size_t>(out, pay->value.size());
    out.write(pay->value.c_str(), pay->value.size());

    // saw payloads
    meta_.features.add<payload>();
  }

  where = out.pool_offset();
  p.pos = pos_;
}

field_data::field_data( 
    const string_ref& name,
    byte_block_pool::inserter* byte_writer,
    int_block_pool::inserter* int_writer )
  : meta_(name, flags::empty_instance()),
    terms_(*byte_writer),
    byte_writer_(byte_writer),
    int_writer_(int_writer),
    last_doc_(doc_limits::invalid()) {
  assert(byte_writer_);
  assert(int_writer_);
}

void field_data::reset(doc_id_t doc_id) {
  assert(doc_limits::valid(doc_id));

  if (doc_id == last_doc_) {
    return; // nothing to do
  }

  pos_ = integer_traits<uint32_t>::const_max;
  last_pos_ = 0;
  len_ = 0;
  num_overlap_ = 0;
  offs_ = 0;
  last_start_offs_ = 0;
  max_term_freq_ = 0;
  unq_term_cnt_ = 0;
  last_doc_ = doc_id;
}

data_output& field_data::norms(columnstore_writer& writer) {
  if (!norms_) {
    auto handle = writer.push_column();
    norms_ = std::move(handle.second);
    meta_.norm = handle.first;
  }

  return norms_(doc());
}

void field_data::new_term(
  posting& p, doc_id_t did, const payload* pay, const offset* offs 
) {
  // where pointers to data starts
  p.int_start = int_writer_->pool_offset();

  const auto freq_start = byte_writer_->alloc_slice(); // pointer to freq stream
  const auto prox_start = byte_writer_->alloc_slice(); // pointer to prox stream
  *int_writer_ = freq_start; // freq stream end
  *int_writer_ = prox_start; // prox stream end
  *int_writer_ = freq_start; // freq stream start
  *int_writer_ = prox_start; // prox stream start

  auto& features = meta_.features;

  p.doc = did;
  if (!features.check<frequency>()) {
    p.doc_code = did;
  } else {
    p.doc_code = uint64_t(did) << 1;
    p.freq = 1;

    if (features.check<position>()) {
      auto& where = *int_writer_->parent().seek(p.int_start + 1);
      write_prox(p, where, pos_, pay);
      if (features.check<offset>()) {
        assert(offs);
        write_offset(p, where, *offs);
      }
    }
  }

  max_term_freq_ = std::max(1U, max_term_freq_);
  ++unq_term_cnt_;
}

void field_data::add_term(
    posting& p,
    doc_id_t did,
    const payload* pay,
    const offset* offs
) {

  auto& features = meta_.features;
  if (!features.check<frequency>()) {
    if (p.doc != did) {
      assert(did > p.doc);

      auto& doc_stream_end = *int_writer_->parent().seek(p.int_start);
      byte_block_pool::sliced_inserter out(byte_writer_, doc_stream_end);
      irs::vwrite<uint32_t>(out, doc_id_t(p.doc_code));
      doc_stream_end = out.pool_offset();

      p.doc_code = did - p.doc;
      p.doc = did;
      ++unq_term_cnt_;
    }
  } else if (p.doc != did) {
    assert(did > p.doc);

    auto& doc_stream_end = *int_writer_->parent().seek(p.int_start);
    byte_block_pool::sliced_inserter out(byte_writer_, doc_stream_end);

    if (1U == p.freq) {
      irs::vwrite<uint64_t>(out, p.doc_code | UINT64_C(1));
    } else {
      irs::vwrite<uint64_t>(out, p.doc_code);
      irs::vwrite<uint32_t>(out, p.freq);
    }

    doc_stream_end = out.pool_offset();

    p.doc_code = uint64_t(did - p.doc) << 1;
    p.freq = 1;

    p.doc = did;
    max_term_freq_ = std::max(1U, max_term_freq_);
    ++unq_term_cnt_;

    if (features.check<position>()) {
      auto& prox_stream_end = *int_writer_->parent().seek(p.int_start+1);
      write_prox(p, prox_stream_end, pos_, pay);
      if (features.check<offset>()) {
        assert(offs);
        p.offs = 0;
        write_offset(p, prox_stream_end, *offs);
      }
    }
  } else { // exists in current doc
    max_term_freq_ = std::max(++p.freq, max_term_freq_);
    if (features.check<position>() ) {
      auto& prox_stream_end = *int_writer_->parent().seek(p.int_start+1);
      write_prox(p, prox_stream_end, pos_ - p.pos, pay);
      if (features.check<offset>()) {
        assert(offs);
        write_offset(p, prox_stream_end, *offs);
      }
    }

  }
}

bool field_data::invert(
    token_stream& stream, 
    const flags& features, 
    doc_id_t id) {
  assert(id < doc_limits::eof()); // 0-based document id
  REGISTER_TIMER_DETAILED();

  meta_.features |= features; // accumulate field features

  auto& attrs = stream.attributes();
  auto& term = attrs.get<term_attribute>();
  auto& inc = attrs.get<increment>();
  const offset* offs = nullptr;
  const payload* pay = nullptr;

  if (!inc) {
    IR_FRMT_ERROR(
      "field '%s' missing required token_stream attribute '%s'",
      meta_.name.c_str(), increment::type().name().c_str()
    );
    return false;
  }

  if (!term) {
    IR_FRMT_ERROR(
      "field '%s' missing required token_stream attribute '%s'",
      meta_.name.c_str(), term_attribute::type().name().c_str()
    );
    return false;
  }

  if (meta_.features.check<offset>()) {
    offs = attrs.get<offset>().get();

    if (offs) {
      pay = attrs.get<payload>().get();
    }
  } 

  reset(id); // initialize field_data for the supplied doc_id

  while (stream.next()) {
    pos_ += inc->value;

    if (pos_ < last_pos_) {
      IR_FRMT_ERROR("invalid position %u < %u in field '%s'", pos_, last_pos_, meta_.name.c_str());
      return false;
    }

    if (pos_ >= pos_limits::eof()) {
      IR_FRMT_ERROR("invalid position %u >= %u in field '%s'", pos_, pos_limits::eof(), meta_.name.c_str());
      return false;
    }

    if (0 == inc->value) {
      ++num_overlap_;
    }

    if (offs) {
      const uint32_t start_offset = offs_ + offs->start;
      const uint32_t end_offset = offs_ + offs->end;

      if (start_offset < last_start_offs_ || end_offset < start_offset) {
        IR_FRMT_ERROR("invalid offset start=%u end=%u in field '%s'", start_offset, end_offset, meta_.name.c_str());
        return false;
      }

      last_start_offs_ = start_offset;
    }

    const auto res = terms_.emplace(term->value());

    if (terms_.end() == res.first) {
      IR_FRMT_ERROR("field '%s' has invalid term '%s'", meta_.name.c_str(), ref_cast<char>(term->value()).c_str());
      continue;
    }

    if (res.second) {
      new_term(res.first->second, id, pay, offs);
    } else {
      add_term(res.first->second, id, pay, offs);
    }

    if (0 == ++len_) {
      IR_FRMT_ERROR(
        "too many tokens in field '%s', document '" IR_UINT32_T_SPECIFIER "'",
         meta_.name.c_str(), id
      );
      return false;
    }

    last_pos_ = pos_;
  }

  if (offs) {
    offs_ += offs->end;
  }

  return true;
}

/* -------------------------------------------------------------------
 * fields_data
 * ------------------------------------------------------------------*/

fields_data::fields_data(const comparer* comparator /*= nullptr*/)
  : comparator_(comparator),
    byte_writer_(byte_pool_.begin()),
    int_writer_(int_pool_.begin()) {
}

field_data& fields_data::emplace(const hashed_string_ref& name) {
  static auto generator = [](
      const hashed_string_ref& key,
      const field_data& value) NOEXCEPT {
    // reuse hash but point ref at value
    return hashed_string_ref(key.hash(), value.meta().name);
  };

  // replace original reference to 'name' provided by the caller
  // with a reference to the cached copy in 'value'
  return map_utils::try_emplace_update_key(
    fields_,                                    // container
    generator,                                  // key generator
    name,                                       // key
    name, &byte_writer_, &int_writer_           // value
  ).first->second;
}

void fields_data::flush(field_writer& fw, flush_state& state) {
  REGISTER_TIMER_DETAILED();

  state.features = &features_;

  struct less_t {
    bool operator()(
        const field_data* lhs,
        const field_data* rhs
    ) const NOEXCEPT {
      return lhs->meta().name < rhs->meta().name;
    };
  };

  std::set<const field_data*, less_t> fields;

  // ensure fields are sorted
  for (auto& entry : fields_) {
    fields.emplace(&entry.second);
  }

  fw.prepare(state);

  detail::term_reader terms;

  for (auto* field : fields) {
    auto& meta = field->meta();

    // reset reader
    terms.reset(*field, state.docmap);

    // write inverted data
    auto it = terms.iterator();
    fw.write(meta.name, meta.norm, meta.features, *it);
  }

  fw.end();
}

void fields_data::reset() NOEXCEPT {
  byte_writer_ = byte_pool_.begin(); // reset position pointer to start of pool
  features_.clear();
  fields_.clear();
  int_writer_ = int_pool_.begin(); // reset position pointer to start of pool
}

NS_END // ROOT

// -----------------------------------------------------------------------------
// --SECTION--                                                       END-OF-FILE
// -----------------------------------------------------------------------------
