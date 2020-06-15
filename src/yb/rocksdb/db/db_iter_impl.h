//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.
//
// The following only applies to changes made to this file as part of YugaByte development.
//
// Portions Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef YB_ROCKSDB_DB_DB_ITER_IMPL_H
#define YB_ROCKSDB_DB_DB_ITER_IMPL_H

#include "yb/rocksdb/iterator.h"
#include "yb/rocksdb/table/internal_iterator.h"
#include "yb/rocksdb/options.h"

#include "yb/rocksdb/db/dbformat.h"
#include "yb/rocksdb/db/filename.h"
#include "yb/rocksdb/port/port.h"
#include "yb/rocksdb/env.h"
#include "yb/rocksdb/iterator.h"
#include "yb/rocksdb/merge_operator.h"
#include "yb/rocksdb/options.h"
#include "yb/rocksdb/table/internal_iterator.h"
#include "yb/rocksdb/util/arena.h"
#include "yb/rocksdb/util/logging.h"
#include "yb/rocksdb/util/mutexlock.h"
#include "yb/rocksdb/util/perf_context_imp.h"
#include "yb/util/string_util.h"

namespace rocksdb {

// Memtables and sstables that make the DB representation contain
// (userkey,seq,type) => uservalue entries.  DBIter
// combines multiple entries for the same userkey found in the DB
// representation into a single entry while accounting for sequence
// numbers, deletion markers, overwrites, etc.
class DBIter : public Iterator {
 public:
  // The following is grossly complicated. TODO: clean it up
  // Which direction is the iterator currently moving?
  // (1) When moving forward, the internal iterator is positioned at
  //     the exact entry that yields this->key(), this->value()
  // (2) When moving backwards, the internal iterator is positioned
  //     just before all entries whose user key == this->key().
  enum Direction {
    kForward,
    kReverse
  };

  DBIter(Env* env, const ImmutableCFOptions& ioptions, const Comparator* cmp,
         InternalIterator* iter, SequenceNumber s, bool arena_mode,
         uint64_t max_sequential_skip_in_iterations, uint64_t version_number,
         const Slice* iterate_upper_bound = nullptr,
         bool prefix_same_as_start = false)
      : arena_mode_(arena_mode),
        env_(env),
        logger_(ioptions.info_log),
        user_comparator_(cmp),
        user_merge_operator_(ioptions.merge_operator),
        iter_(iter),
        sequence_(s),
        direction_(kForward),
        valid_(false),
        current_entry_is_merged_(false),
        statistics_(ioptions.statistics),
        version_number_(version_number),
        iterate_upper_bound_(iterate_upper_bound),
        prefix_same_as_start_(prefix_same_as_start),
        iter_pinned_(false) {
    RecordTick(statistics_, NO_ITERATORS);
    prefix_extractor_ = ioptions.prefix_extractor;
    max_skip_ = max_sequential_skip_in_iterations;
  }
  virtual ~DBIter() {
    RecordTick(statistics_, NO_ITERATORS, -1);
    if (!arena_mode_) {
      delete iter_;
    } else {
      iter_->~InternalIterator();
    }
  }
  virtual void SetIter(InternalIterator* iter) {
    assert(iter_ == nullptr);
    iter_ = iter;
    if (iter_ && iter_pinned_) {
      CHECK_OK(iter_->PinData());
    }
  }
  bool Valid() const override { return valid_; }
  Slice key() const override {
    assert(valid_);
    return saved_key_.GetKey();
  }
  Slice value() const override {
    assert(valid_);
    return (direction_ == kForward && !current_entry_is_merged_) ?
      iter_->value() : saved_value_;
  }
  Status status() const override {
    if (status_.ok()) {
      return iter_->status();
    } else {
      return status_;
    }
  }
  virtual Status PinData() {
    Status s;
    if (iter_) {
      s = iter_->PinData();
    }
    if (s.ok()) {
      // Even if iter_ is nullptr, we set iter_pinned_ to true so that when
      // iter_ is updated using SetIter, we Pin it.
      iter_pinned_ = true;
    }
    return s;
  }
  virtual Status ReleasePinnedData() {
    Status s;
    if (iter_) {
      s = iter_->ReleasePinnedData();
    }
    if (s.ok()) {
      iter_pinned_ = false;
    }
    return s;
  }

  virtual Status GetProperty(std::string prop_name,
                             std::string* prop) override {
    if (prop == nullptr) {
      return STATUS(InvalidArgument, "prop is nullptr");
    }
    if (prop_name == "rocksdb.iterator.super-version-number") {
      // First try to pass the value returned from inner iterator.
      if (!iter_->GetProperty(prop_name, prop).ok()) {
        *prop = ToString(version_number_);
      }
      return Status::OK();
    } else if (prop_name == "rocksdb.iterator.is-key-pinned") {
      if (valid_) {
        *prop = (iter_pinned_ && saved_key_.IsKeyPinned()) ? "1" : "0";
      } else {
        *prop = "Iterator is not valid.";
      }
      return Status::OK();
    }
    return STATUS(InvalidArgument, "Undentified property.");
  }

  void Next() override;
  void Prev() override;
  void Seek(const Slice& target) override;
  void SeekToFirst() override;
  void SeekToLast() override;

  void RevalidateAfterUpperBoundChange() override {
    if (iter_->Valid() && direction_ == kForward) {
      valid_ = true;
      FindNextUserEntry(/* skipping= */ false);
    }
  }

 private:
  void ReverseToBackward();
  void PrevInternal();
  void FindParseableKey(ParsedInternalKey* ikey, Direction direction);
  bool FindValueForCurrentKey();
  bool FindValueForCurrentKeyUsingSeek();
  void FindPrevUserKey();
  void FindNextUserKey();
  inline void FindNextUserEntry(bool skipping);
  void FindNextUserEntryInternal(bool skipping);
  bool ParseKey(ParsedInternalKey* key);
  void MergeValuesNewToOld();

  inline void ClearSavedValue() {
    if (saved_value_.capacity() > 1048576) {
      std::string empty;
      swap(empty, saved_value_);
    } else {
      saved_value_.clear();
    }
  }

  const SliceTransform* prefix_extractor_;
  bool arena_mode_;
  Env* const env_;
  Logger* logger_;
  const Comparator* const user_comparator_;
  const MergeOperator* const user_merge_operator_;
  InternalIterator* iter_;
  SequenceNumber const sequence_;

  Status status_;
  IterKey saved_key_;
  std::string saved_value_;
  Direction direction_;
  bool valid_;
  bool current_entry_is_merged_;
  Statistics* statistics_;
  uint64_t max_skip_;
  uint64_t version_number_;
  const Slice* iterate_upper_bound_;
  IterKey prefix_start_;
  bool prefix_same_as_start_;
  bool iter_pinned_;
  // List of operands for merge operator.
  std::deque<std::string> merge_operands_;

  // No copying allowed
  DBIter(const DBIter&);
  void operator=(const DBIter&);
};

}  // namespace rocksdb

#endif  // YB_ROCKSDB_DB_DB_ITER_IMPL_H