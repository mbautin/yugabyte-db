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

#include "yb/rocksdb/db/db_iter.h"
#include "yb/rocksdb/db/db_iter_impl.h"

#include <stdexcept>
#include <deque>
#include <string>
#include <limits>

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

// #include "yb/rocksdb/db/simple_db_iter.h"

namespace rocksdb {

inline bool DBIter::ParseKey(ParsedInternalKey* ikey) {
  if (!ParseInternalKey(iter_->key(), ikey)) {
    status_ = STATUS(Corruption, "corrupted internal key in DBIter");
    RLOG(InfoLogLevel::ERROR_LEVEL,
        logger_, "corrupted internal key in DBIter: %s",
        iter_->key().ToString(true).c_str());
    return false;
  } else {
    return true;
  }
}

void DBIter::Next() {
  assert(valid_);

  if (direction_ == kReverse) {
    FindNextUserKey();
    direction_ = kForward;
    if (!iter_->Valid()) {
      iter_->SeekToFirst();
    }
  } else if (iter_->Valid() && !current_entry_is_merged_) {
    // If the current value is not a merge, the iter position is the
    // current key, which is already returned. We can safely issue a
    // Next() without checking the current key.
    // If the current key is a merge, very likely iter already points
    // to the next internal position.
    iter_->Next();
  }

  // Now we point to the next internal position, for both of merge and
  // not merge cases.
  if (!iter_->Valid()) {
    valid_ = false;
    return;
  }
  FindNextUserEntry(true /* skipping the current user key */);
  if (statistics_ != nullptr) {
    RecordTick(statistics_, NUMBER_DB_NEXT);
    if (valid_) {
      RecordTick(statistics_, NUMBER_DB_NEXT_FOUND);
      RecordTick(statistics_, ITER_BYTES_READ, key().size() + value().size());
    }
  }
  if (valid_ && prefix_extractor_ && prefix_same_as_start_ &&
      prefix_extractor_->Transform(saved_key_.GetKey())
              .compare(prefix_start_.GetKey()) != 0) {
    valid_ = false;
  }
}

// PRE: saved_key_ has the current user key if skipping
// POST: saved_key_ should have the next user key if valid_,
//       if the current entry is a result of merge
//           current_entry_is_merged_ => true
//           saved_value_             => the merged value
//
// NOTE: In between, saved_key_ can point to a user key that has
//       a delete marker
inline void DBIter::FindNextUserEntry(bool skipping) {
  PERF_TIMER_GUARD(find_next_user_entry_time);
  FindNextUserEntryInternal(skipping);
}

// Actual implementation of DBIter::FindNextUserEntry()
void DBIter::FindNextUserEntryInternal(bool skipping) {
  // Loop until we hit an acceptable entry to yield
  assert(iter_->Valid());
  assert(direction_ == kForward);
  current_entry_is_merged_ = false;
  uint64_t num_skipped = 0;
  do {
    ParsedInternalKey ikey;

    if (ParseKey(&ikey)) {
      if (iterate_upper_bound_ != nullptr &&
          user_comparator_->Compare(ikey.user_key, *iterate_upper_bound_) >= 0) {
        break;
      }

      if (ikey.sequence <= sequence_) {
        if (skipping &&
           user_comparator_->Compare(ikey.user_key, saved_key_.GetKey()) <= 0) {
          num_skipped++;  // skip this entry
          PERF_COUNTER_ADD(internal_key_skipped_count, 1);
        } else {
          switch (ikey.type) {
            case kTypeDeletion:
            case kTypeSingleDeletion:
              // Arrange to skip all upcoming entries for this key since
              // they are hidden by this deletion.
              saved_key_.SetKey(ikey.user_key,
                                !iter_->IsKeyPinned() /* copy */);
              skipping = true;
              num_skipped = 0;
              PERF_COUNTER_ADD(internal_delete_skipped_count, 1);
              break;
            case kTypeValue:
              valid_ = true;
              saved_key_.SetKey(ikey.user_key,
                                !iter_->IsKeyPinned() /* copy */);
              return;
            case kTypeMerge:
              // By now, we are sure the current ikey is going to yield a value
              saved_key_.SetKey(ikey.user_key,
                                !iter_->IsKeyPinned() /* copy */);
              current_entry_is_merged_ = true;
              valid_ = true;
              MergeValuesNewToOld();  // Go to a different state machine
              return;
            default:
              assert(false);
              break;
          }
        }
      }
    }
    // If we have sequentially iterated via numerous keys and still not
    // found the next user-key, then it is better to seek so that we can
    // avoid too many key comparisons. We seek to the last occurrence of
    // our current key by looking for sequence number 0 and type deletion
    // (the smallest type).
    if (skipping && num_skipped > max_skip_) {
      num_skipped = 0;
      std::string last_key;
      AppendInternalKey(&last_key, ParsedInternalKey(saved_key_.GetKey(), 0,
                                                     kTypeDeletion));
      iter_->Seek(last_key);
      RecordTick(statistics_, NUMBER_OF_RESEEKS_IN_ITERATION);
    } else {
      iter_->Next();
    }
  } while (iter_->Valid());
  valid_ = false;
}

// Merge values of the same user key starting from the current iter_ position
// Scan from the newer entries to older entries.
// PRE: iter_->key() points to the first merge type entry
//      saved_key_ stores the user key
// POST: saved_value_ has the merged value for the user key
//       iter_ points to the next entry (or invalid)
void DBIter::MergeValuesNewToOld() {
  if (!user_merge_operator_) {
    RLOG(InfoLogLevel::ERROR_LEVEL,
        logger_, "Options::merge_operator is null.");
    status_ = STATUS(InvalidArgument, "user_merge_operator_ must be set.");
    valid_ = false;
    return;
  }

  // Start the merge process by pushing the first operand
  std::deque<std::string> operands;
  operands.push_front(iter_->value().ToString());

  ParsedInternalKey ikey;
  for (iter_->Next(); iter_->Valid(); iter_->Next()) {
    if (!ParseKey(&ikey)) {
      // skip corrupted key
      continue;
    }

    if (!user_comparator_->Equal(ikey.user_key, saved_key_.GetKey())) {
      // hit the next user key, stop right here
      break;
    } else if (kTypeDeletion == ikey.type || kTypeSingleDeletion == ikey.type) {
      // hit a delete with the same user key, stop right here
      // iter_ is positioned after delete
      iter_->Next();
      break;
    } else if (kTypeValue == ikey.type) {
      // hit a put, merge the put value with operands and store the
      // final result in saved_value_. We are done!
      // ignore corruption if there is any.
      const Slice val = iter_->value();
      {
        StopWatchNano timer(env_, statistics_ != nullptr);
        PERF_TIMER_GUARD(merge_operator_time_nanos);
        user_merge_operator_->FullMerge(ikey.user_key, &val, operands,
                                        &saved_value_, logger_);
        RecordTick(statistics_, MERGE_OPERATION_TOTAL_TIME,
                   timer.ElapsedNanos());
      }
      // iter_ is positioned after put
      iter_->Next();
      return;
    } else if (kTypeMerge == ikey.type) {
      // hit a merge, add the value as an operand and run associative merge.
      // when complete, add result to operands and continue.
      const Slice& val = iter_->value();
      operands.push_front(val.ToString());
    } else {
      assert(false);
    }
  }

  {
    StopWatchNano timer(env_, statistics_ != nullptr);
    PERF_TIMER_GUARD(merge_operator_time_nanos);
    // we either exhausted all internal keys under this user key, or hit
    // a deletion marker.
    // feed null as the existing value to the merge operator, such that
    // client can differentiate this scenario and do things accordingly.
    user_merge_operator_->FullMerge(saved_key_.GetKey(), nullptr, operands,
                                    &saved_value_, logger_);
    RecordTick(statistics_, MERGE_OPERATION_TOTAL_TIME, timer.ElapsedNanos());
  }
}

void DBIter::Prev() {
  assert(valid_);
  if (direction_ == kForward) {
    ReverseToBackward();
  }
  PrevInternal();
  if (statistics_ != nullptr) {
    RecordTick(statistics_, NUMBER_DB_PREV);
    if (valid_) {
      RecordTick(statistics_, NUMBER_DB_PREV_FOUND);
      RecordTick(statistics_, ITER_BYTES_READ, key().size() + value().size());
    }
  }
  if (valid_ && prefix_extractor_ && prefix_same_as_start_ &&
      prefix_extractor_->Transform(saved_key_.GetKey())
              .compare(prefix_start_.GetKey()) != 0) {
    valid_ = false;
  }
}

void DBIter::ReverseToBackward() {
  if (current_entry_is_merged_) {
    // Not placed in the same key. Need to call Prev() until finding the
    // previous key.
    if (!iter_->Valid()) {
      iter_->SeekToLast();
    }
    ParsedInternalKey ikey;
    FindParseableKey(&ikey, kReverse);
    while (iter_->Valid() &&
           user_comparator_->Compare(ikey.user_key, saved_key_.GetKey()) > 0) {
      iter_->Prev();
      FindParseableKey(&ikey, kReverse);
    }
  }
#ifndef NDEBUG
  if (iter_->Valid()) {
    ParsedInternalKey ikey;
    assert(ParseKey(&ikey));
    assert(user_comparator_->Compare(ikey.user_key, saved_key_.GetKey()) <= 0);
  }
#endif

  FindPrevUserKey();
  direction_ = kReverse;
}

void DBIter::PrevInternal() {
  if (!iter_->Valid()) {
    valid_ = false;
    return;
  }

  ParsedInternalKey ikey;

  while (iter_->Valid()) {
    saved_key_.SetKey(ExtractUserKey(iter_->key()),
                      !iter_->IsKeyPinned() /* copy */);
    if (FindValueForCurrentKey()) {
      valid_ = true;
      if (!iter_->Valid()) {
        return;
      }
      FindParseableKey(&ikey, kReverse);
      if (user_comparator_->Equal(ikey.user_key, saved_key_.GetKey())) {
        FindPrevUserKey();
      }
      return;
    }
    if (!iter_->Valid()) {
      break;
    }
    FindParseableKey(&ikey, kReverse);
    if (user_comparator_->Equal(ikey.user_key, saved_key_.GetKey())) {
      FindPrevUserKey();
    }
  }
  // We haven't found any key - iterator is not valid
  assert(!iter_->Valid());
  valid_ = false;
}

// This function checks, if the entry with biggest sequence_number <= sequence_
// is non kTypeDeletion or kTypeSingleDeletion. If it's not, we save value in
// saved_value_
bool DBIter::FindValueForCurrentKey() {
  assert(iter_->Valid());
  merge_operands_.clear();
  // last entry before merge (could be kTypeDeletion, kTypeSingleDeletion or
  // kTypeValue)
  ValueType last_not_merge_type = kTypeDeletion;
  ValueType last_key_entry_type = kTypeDeletion;

  ParsedInternalKey ikey;
  FindParseableKey(&ikey, kReverse);

  size_t num_skipped = 0;
  while (iter_->Valid() && ikey.sequence <= sequence_ &&
         user_comparator_->Equal(ikey.user_key, saved_key_.GetKey())) {
    // We iterate too much: let's use Seek() to avoid too much key comparisons
    if (num_skipped >= max_skip_) {
      return FindValueForCurrentKeyUsingSeek();
    }

    last_key_entry_type = ikey.type;
    switch (last_key_entry_type) {
      case kTypeValue:
        merge_operands_.clear();
        saved_value_ = iter_->value().ToString();
        last_not_merge_type = kTypeValue;
        break;
      case kTypeDeletion:
      case kTypeSingleDeletion:
        merge_operands_.clear();
        last_not_merge_type = last_key_entry_type;
        PERF_COUNTER_ADD(internal_delete_skipped_count, 1);
        break;
      case kTypeMerge:
        assert(user_merge_operator_ != nullptr);
        merge_operands_.push_back(iter_->value().ToString());
        break;
      default:
        assert(false);
    }

    PERF_COUNTER_ADD(internal_key_skipped_count, 1);
    assert(user_comparator_->Equal(ikey.user_key, saved_key_.GetKey()));
    iter_->Prev();
    ++num_skipped;
    FindParseableKey(&ikey, kReverse);
  }

  switch (last_key_entry_type) {
    case kTypeDeletion:
    case kTypeSingleDeletion:
      valid_ = false;
      return false;
    case kTypeMerge:
      if (last_not_merge_type == kTypeDeletion) {
        StopWatchNano timer(env_, statistics_ != nullptr);
        PERF_TIMER_GUARD(merge_operator_time_nanos);
        user_merge_operator_->FullMerge(saved_key_.GetKey(), nullptr,
                                        merge_operands_, &saved_value_,
                                        logger_);
        RecordTick(statistics_, MERGE_OPERATION_TOTAL_TIME,
                   timer.ElapsedNanos());
      } else {
        assert(last_not_merge_type == kTypeValue);
        std::string last_put_value = saved_value_;
        Slice temp_slice(last_put_value);
        {
          StopWatchNano timer(env_, statistics_ != nullptr);
          PERF_TIMER_GUARD(merge_operator_time_nanos);
          user_merge_operator_->FullMerge(saved_key_.GetKey(), &temp_slice,
                                          merge_operands_, &saved_value_,
                                          logger_);
          RecordTick(statistics_, MERGE_OPERATION_TOTAL_TIME,
                     timer.ElapsedNanos());
        }
      }
      break;
    case kTypeValue:
      // do nothing - we've already has value in saved_value_
      break;
    default:
      assert(false);
      break;
  }
  valid_ = true;
  return true;
}

// This function is used in FindValueForCurrentKey.
// We use Seek() function instead of Prev() to find necessary value
bool DBIter::FindValueForCurrentKeyUsingSeek() {
  std::string last_key;
  AppendInternalKey(&last_key, ParsedInternalKey(saved_key_.GetKey(), sequence_,
                                                 kValueTypeForSeek));
  iter_->Seek(last_key);
  RecordTick(statistics_, NUMBER_OF_RESEEKS_IN_ITERATION);

  // assume there is at least one parseable key for this user key
  ParsedInternalKey ikey;
  FindParseableKey(&ikey, kForward);

  if (ikey.type == kTypeValue || ikey.type == kTypeDeletion ||
      ikey.type == kTypeSingleDeletion) {
    if (ikey.type == kTypeValue) {
      saved_value_ = iter_->value().ToString();
      valid_ = true;
      return true;
    }
    valid_ = false;
    return false;
  }

  // kTypeMerge. We need to collect all kTypeMerge values and save them
  // in operands
  std::deque<std::string> operands;
  while (iter_->Valid() &&
         user_comparator_->Equal(ikey.user_key, saved_key_.GetKey()) &&
         ikey.type == kTypeMerge) {
    operands.push_front(iter_->value().ToString());
    iter_->Next();
    FindParseableKey(&ikey, kForward);
  }

  if (!iter_->Valid() ||
      !user_comparator_->Equal(ikey.user_key, saved_key_.GetKey()) ||
      ikey.type == kTypeDeletion || ikey.type == kTypeSingleDeletion) {
    {
      StopWatchNano timer(env_, statistics_ != nullptr);
      PERF_TIMER_GUARD(merge_operator_time_nanos);
      user_merge_operator_->FullMerge(saved_key_.GetKey(), nullptr, operands,
                                      &saved_value_, logger_);
      RecordTick(statistics_, MERGE_OPERATION_TOTAL_TIME, timer.ElapsedNanos());
    }
    // Make iter_ valid and point to saved_key_
    if (!iter_->Valid() ||
        !user_comparator_->Equal(ikey.user_key, saved_key_.GetKey())) {
      iter_->Seek(last_key);
      RecordTick(statistics_, NUMBER_OF_RESEEKS_IN_ITERATION);
    }
    valid_ = true;
    return true;
  }

  const Slice& val = iter_->value();
  {
    StopWatchNano timer(env_, statistics_ != nullptr);
    PERF_TIMER_GUARD(merge_operator_time_nanos);
    user_merge_operator_->FullMerge(saved_key_.GetKey(), &val, operands,
                                    &saved_value_, logger_);
    RecordTick(statistics_, MERGE_OPERATION_TOTAL_TIME, timer.ElapsedNanos());
  }
  valid_ = true;
  return true;
}

// Used in Next to change directions
// Go to next user key
// Don't use Seek(),
// because next user key will be very close
void DBIter::FindNextUserKey() {
  if (!iter_->Valid()) {
    return;
  }
  ParsedInternalKey ikey;
  FindParseableKey(&ikey, kForward);
  while (iter_->Valid() &&
         !user_comparator_->Equal(ikey.user_key, saved_key_.GetKey())) {
    iter_->Next();
    FindParseableKey(&ikey, kForward);
  }
}

// Go to previous user_key
void DBIter::FindPrevUserKey() {
  if (!iter_->Valid()) {
    return;
  }
  size_t num_skipped = 0;
  ParsedInternalKey ikey;
  FindParseableKey(&ikey, kReverse);
  int cmp;
  while (iter_->Valid() && ((cmp = user_comparator_->Compare(
                                 ikey.user_key, saved_key_.GetKey())) == 0 ||
                            (cmp > 0 && ikey.sequence > sequence_))) {
    if (cmp == 0) {
      if (num_skipped >= max_skip_) {
        num_skipped = 0;
        IterKey last_key;
        last_key.SetInternalKey(ParsedInternalKey(
            saved_key_.GetKey(), kMaxSequenceNumber, kValueTypeForSeek));
        iter_->Seek(last_key.GetKey());
        RecordTick(statistics_, NUMBER_OF_RESEEKS_IN_ITERATION);
      } else {
        ++num_skipped;
      }
    }
    iter_->Prev();
    FindParseableKey(&ikey, kReverse);
  }
}

// Skip all unparseable keys
void DBIter::FindParseableKey(ParsedInternalKey* ikey, Direction direction) {
  while (iter_->Valid() && !ParseKey(ikey)) {
    if (direction == kReverse) {
      iter_->Prev();
    } else {
      iter_->Next();
    }
  }
}

void DBIter::Seek(const Slice& target) {
  saved_key_.Clear();
  // now savved_key is used to store internal key.
  saved_key_.SetInternalKey(target, sequence_);

  {
    PERF_TIMER_GUARD(seek_internal_seek_time);
    iter_->Seek(saved_key_.GetKey());
  }

  RecordTick(statistics_, NUMBER_DB_SEEK);
  if (iter_->Valid()) {
    direction_ = kForward;
    ClearSavedValue();
    FindNextUserEntry(false /* not skipping */);
    if (statistics_ != nullptr) {
      if (valid_) {
        RecordTick(statistics_, NUMBER_DB_SEEK_FOUND);
        RecordTick(statistics_, ITER_BYTES_READ, key().size() + value().size());
      }
    }
  } else {
    valid_ = false;
  }
  if (valid_ && prefix_extractor_ && prefix_same_as_start_) {
    prefix_start_.SetKey(prefix_extractor_->Transform(target));
  }
}

void DBIter::SeekToFirst() {
  // Don't use iter_::Seek() if we set a prefix extractor
  // because prefix seek will be used.
  if (prefix_extractor_ != nullptr) {
    max_skip_ = std::numeric_limits<uint64_t>::max();
  }
  direction_ = kForward;
  ClearSavedValue();

  {
    PERF_TIMER_GUARD(seek_internal_seek_time);
    iter_->SeekToFirst();
  }

  RecordTick(statistics_, NUMBER_DB_SEEK);
  if (iter_->Valid()) {
    FindNextUserEntry(false /* not skipping */);
    if (statistics_ != nullptr) {
      if (valid_) {
        RecordTick(statistics_, NUMBER_DB_SEEK_FOUND);
        RecordTick(statistics_, ITER_BYTES_READ, key().size() + value().size());
      }
    }
  } else {
    valid_ = false;
  }
  if (valid_ && prefix_extractor_ && prefix_same_as_start_) {
    prefix_start_.SetKey(prefix_extractor_->Transform(saved_key_.GetKey()));
  }
}

void DBIter::SeekToLast() {
  // Don't use iter_::Seek() if we set a prefix extractor
  // because prefix seek will be used.
  if (prefix_extractor_ != nullptr) {
    max_skip_ = std::numeric_limits<uint64_t>::max();
  }
  direction_ = kReverse;
  ClearSavedValue();

  {
    PERF_TIMER_GUARD(seek_internal_seek_time);
    iter_->SeekToLast();
  }
  // When the iterate_upper_bound is set to a value,
  // it will seek to the last key before the
  // ReadOptions.iterate_upper_bound
  if (iter_->Valid() && iterate_upper_bound_ != nullptr) {
    saved_key_.SetKey(*iterate_upper_bound_, false /* copy */);
    std::string last_key;
    AppendInternalKey(&last_key,
                      ParsedInternalKey(saved_key_.GetKey(), kMaxSequenceNumber,
                                        kValueTypeForSeek));

    iter_->Seek(last_key);

    if (!iter_->Valid()) {
      iter_->SeekToLast();
    } else {
      iter_->Prev();
      if (!iter_->Valid()) {
        valid_ = false;
        return;
      }
    }
  }
  PrevInternal();
  if (statistics_ != nullptr) {
    RecordTick(statistics_, NUMBER_DB_SEEK);
    if (valid_) {
      RecordTick(statistics_, NUMBER_DB_SEEK_FOUND);
      RecordTick(statistics_, ITER_BYTES_READ, key().size() + value().size());
    }
  }
  if (valid_ && prefix_extractor_ && prefix_same_as_start_) {
    prefix_start_.SetKey(prefix_extractor_->Transform(saved_key_.GetKey()));
  }
}

void DBIter::RevalidateAfterUpperBoundChange() {
  if (iter_->Valid() && direction_ == kForward) {
    valid_ = true;
    FindNextUserEntry(/* skipping= */ false);
  }
}


// ------------------------------------------------------------------------------------------------
// NewDBiterator factory function
// ------------------------------------------------------------------------------------------------

Iterator* NewDBIterator(Env* env, const ImmutableCFOptions& ioptions,
                        const Comparator* user_key_comparator,
                        InternalIterator* internal_iter,
                        const SequenceNumber& sequence,
                        uint64_t max_sequential_skip_in_iterations,
                        uint64_t version_number,
                        const Slice* iterate_upper_bound,
                        bool prefix_same_as_start, bool pin_data,
                        bool use_yb_simplified_regular_db_iter) {
  // if (use_yb_simplified_regular_db_iter) {
  //   if (iterate_upper_bound) {
  //     LOG(FATAL) << "iterate_upper_bound is not supported for SimpleDBIter";
  //   }
  //   auto* regular_db_iter =
  //       new SimpleDBIter(
  //           env, ioptions, user_key_comparator, internal_iter, sequence,
  //           false, max_sequential_skip_in_iterations, version_number);
  //   if (pin_data) {
  //     CHECK_OK(regular_db_iter->PinData());
  //   }
  //   return regular_db_iter;
  // }
  DBIter* db_iter =
      new DBIter(env, ioptions, user_key_comparator, internal_iter, sequence,
                false, max_sequential_skip_in_iterations, version_number,
                iterate_upper_bound, prefix_same_as_start);
  if (pin_data) {
    CHECK_OK(db_iter->PinData());
  }
  return db_iter;
}

}  // namespace rocksdb
