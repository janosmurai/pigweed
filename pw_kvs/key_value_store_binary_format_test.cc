// Copyright 2020 The Pigweed Authors
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not
// use this file except in compliance with the License. You may obtain a copy of
// the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
// WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
// License for the specific language governing permissions and limitations under
// the License.

// Tests that directly work with the KVS's binary format and flash layer.

#include <string_view>

#include "gtest/gtest.h"
#include "pw_kvs/crc16_checksum.h"
#include "pw_kvs/format.h"
#include "pw_kvs/in_memory_fake_flash.h"
#include "pw_kvs/internal/hash.h"
#include "pw_kvs/key_value_store.h"
#include "pw_kvs_private/byte_utils.h"

namespace pw::kvs {
namespace {

using std::byte;
using std::string_view;

constexpr size_t kMaxEntries = 256;
constexpr size_t kMaxUsableSectors = 256;

constexpr uint32_t SimpleChecksum(span<const byte> data, uint32_t state) {
  for (byte b : data) {
    state += uint32_t(b);
  }
  return state;
}

template <typename State>
class ChecksumFunction final : public ChecksumAlgorithm {
 public:
  ChecksumFunction(State (&algorithm)(span<const byte>, State))
      : ChecksumAlgorithm(as_bytes(span(&state_, 1))), algorithm_(algorithm) {}

  void Reset() override { state_ = {}; }

  void Update(span<const byte> data) override {
    state_ = algorithm_(data, state_);
  }

 private:
  State state_;
  State (&algorithm_)(span<const byte>, State);
};

ChecksumFunction<uint32_t> checksum(SimpleChecksum);

// Returns a buffer containing the necessary padding for an entry.
template <size_t kAlignmentBytes, size_t kKeyLength, size_t kValueSize>
constexpr auto EntryPadding() {
  constexpr size_t content =
      sizeof(internal::EntryHeader) + kKeyLength + kValueSize;
  return std::array<byte, Padding(content, kAlignmentBytes)>{};
}

// Creates a buffer containing a valid entry at compile time.
template <uint32_t (*kChecksum)(span<const byte>, uint32_t) = &SimpleChecksum,
          size_t kAlignmentBytes = sizeof(internal::EntryHeader),
          size_t kKeyLengthWithNull,
          size_t kValueSize>
constexpr auto MakeValidEntry(uint32_t magic,
                              uint32_t id,
                              const char (&key)[kKeyLengthWithNull],
                              const std::array<byte, kValueSize>& value) {
  constexpr size_t kKeyLength = kKeyLengthWithNull - 1;

  auto data = AsBytes(magic,
                      uint32_t(0),
                      uint8_t(kAlignmentBytes / 16 - 1),
                      uint8_t(kKeyLength),
                      uint16_t(kValueSize),
                      id,
                      ByteStr(key),
                      span(value),
                      EntryPadding<kAlignmentBytes, kKeyLength, kValueSize>());

  // Calculate the checksum
  uint32_t checksum = kChecksum(data, 0);
  for (size_t i = 0; i < sizeof(checksum); ++i) {
    data[4 + i] = byte(checksum & 0xff);
    checksum >>= 8;
  }

  return data;
}

constexpr uint32_t kMagic = 0xc001beef;

constexpr Options kNoGcOptions{
    .gc_on_write = GargbageCollectOnWrite::kDisabled,
    .recovery = ErrorRecovery::kManual,
    .verify_on_read = true,
    .verify_on_write = true,
};

constexpr Options kRecoveryNoGcOptions{
    .gc_on_write = GargbageCollectOnWrite::kDisabled,
    .recovery = ErrorRecovery::kLazy,
    .verify_on_read = true,
    .verify_on_write = true,
};

constexpr Options kRecoveryLazyGcOptions{
    .gc_on_write = GargbageCollectOnWrite::kOneSector,
    .recovery = ErrorRecovery::kLazy,
    .verify_on_read = true,
    .verify_on_write = true,
};

constexpr auto kEntry1 = MakeValidEntry(kMagic, 1, "key1", ByteStr("value1"));
constexpr auto kEntry2 = MakeValidEntry(kMagic, 3, "k2", ByteStr("value2"));
constexpr auto kEntry3 = MakeValidEntry(kMagic, 4, "k3y", ByteStr("value3"));
constexpr auto kEntry4 = MakeValidEntry(kMagic, 5, "4k", ByteStr("value4"));

class KvsErrorHandling : public ::testing::Test {
 protected:
  KvsErrorHandling()
      : flash_(internal::Entry::kMinAlignmentBytes),
        partition_(&flash_),
        kvs_(&partition_,
             {.magic = kMagic, .checksum = &checksum},
             kNoGcOptions) {}

  void InitFlashTo(span<const byte> contents) {
    partition_.Erase();
    std::memcpy(flash_.buffer().data(), contents.data(), contents.size());
  }

  FakeFlashBuffer<512, 4> flash_;
  FlashPartition partition_;
  KeyValueStoreBuffer<kMaxEntries, kMaxUsableSectors> kvs_;
};

TEST_F(KvsErrorHandling, Init_Ok) {
  InitFlashTo(AsBytes(kEntry1, kEntry2));

  EXPECT_EQ(Status::OK, kvs_.Init());
  byte buffer[64];
  EXPECT_EQ(Status::OK, kvs_.Get("key1", buffer).status());
  EXPECT_EQ(Status::OK, kvs_.Get("k2", buffer).status());
}

TEST_F(KvsErrorHandling, Init_DuplicateEntries_ReturnsDataLossButReadsEntry) {
  InitFlashTo(AsBytes(kEntry1, kEntry1));

  EXPECT_EQ(Status::DATA_LOSS, kvs_.Init());
  byte buffer[64];
  EXPECT_EQ(Status::OK, kvs_.Get("key1", buffer).status());
  EXPECT_EQ(Status::NOT_FOUND, kvs_.Get("k2", buffer).status());
}

TEST_F(KvsErrorHandling, Init_CorruptEntry_FindsSubsequentValidEntry) {
  // Corrupt each byte in the first entry once.
  for (size_t i = 0; i < kEntry1.size(); ++i) {
    InitFlashTo(AsBytes(kEntry1, kEntry2));
    flash_.buffer()[i] = byte(int(flash_.buffer()[i]) + 1);

    ASSERT_EQ(Status::DATA_LOSS, kvs_.Init());
    byte buffer[64];
    ASSERT_EQ(Status::NOT_FOUND, kvs_.Get("key1", buffer).status());
    ASSERT_EQ(Status::OK, kvs_.Get("k2", buffer).status());

    auto stats = kvs_.GetStorageStats();
    // One valid entry.
    ASSERT_EQ(32u, stats.in_use_bytes);
    // Rest of space is reclaimable as the sector is corrupt.
    ASSERT_EQ(480u, stats.reclaimable_bytes);
  }
}

TEST_F(KvsErrorHandling, Init_CorruptEntry_CorrectlyAccountsForSectorSize) {
  InitFlashTo(AsBytes(kEntry1, kEntry2, kEntry3, kEntry4));

  // Corrupt the first and third entries.
  flash_.buffer()[9] = byte(0xef);
  flash_.buffer()[67] = byte(0xef);

  ASSERT_EQ(Status::DATA_LOSS, kvs_.Init());

  EXPECT_EQ(2u, kvs_.size());

  byte buffer[64];
  EXPECT_EQ(Status::NOT_FOUND, kvs_.Get("key1", buffer).status());
  EXPECT_EQ(Status::OK, kvs_.Get("k2", buffer).status());
  EXPECT_EQ(Status::NOT_FOUND, kvs_.Get("k3y", buffer).status());
  EXPECT_EQ(Status::OK, kvs_.Get("4k", buffer).status());

  auto stats = kvs_.GetStorageStats();
  ASSERT_EQ(64u, stats.in_use_bytes);
  ASSERT_EQ(448u, stats.reclaimable_bytes);
  ASSERT_EQ(1024u, stats.writable_bytes);
}

TEST_F(KvsErrorHandling, Init_ReadError_InitializedWithSingleEntryError) {
  InitFlashTo(AsBytes(kEntry1, kEntry2));

  flash_.InjectReadError(
      FlashError::InRange(Status::UNAUTHENTICATED, kEntry1.size()));

  EXPECT_EQ(Status::DATA_LOSS, kvs_.Init());
  EXPECT_FALSE(kvs_.initialized());
}

TEST_F(KvsErrorHandling, Init_CorruptSectors_ShouldBeUnwritable) {
  InitFlashTo(AsBytes(kEntry1, kEntry2));

  // Corrupt 3 of the 4 512-byte flash sectors. Corrupt sectors should be
  // unwritable, and the KVS must maintain one empty sector at all times.
  // As GC on write is disabled through KVS options, writes should no longer be
  // possible due to lack of space.
  flash_.buffer()[1] = byte(0xef);
  flash_.buffer()[513] = byte(0xef);
  flash_.buffer()[1025] = byte(0xef);

  ASSERT_EQ(Status::DATA_LOSS, kvs_.Init());
  EXPECT_EQ(Status::FAILED_PRECONDITION, kvs_.Put("hello", ByteStr("world")));
  EXPECT_EQ(Status::FAILED_PRECONDITION, kvs_.Put("a", ByteStr("b")));

  // Existing valid entries should still be readable.
  EXPECT_EQ(1u, kvs_.size());
  byte buffer[64];
  EXPECT_EQ(Status::NOT_FOUND, kvs_.Get("key1", buffer).status());
  EXPECT_EQ(Status::OK, kvs_.Get("k2", buffer).status());

  auto stats = kvs_.GetStorageStats();
  EXPECT_EQ(32u, stats.in_use_bytes);
  EXPECT_EQ(480u + 2 * 512u, stats.reclaimable_bytes);
  EXPECT_EQ(0u, stats.writable_bytes);
}

TEST_F(KvsErrorHandling, Init_CorruptSectors_ShouldRecoverOne) {
  InitFlashTo(AsBytes(kEntry1, kEntry2));

  // Corrupt all of the 4 512-byte flash sectors. Leave the pre-init entries
  // intact. The KVS should be unavailable because recovery is set to full
  // manual, and it does not have the required one empty sector at all times.
  flash_.buffer()[64] = byte(0xef);
  flash_.buffer()[513] = byte(0xef);
  flash_.buffer()[1025] = byte(0xef);
  flash_.buffer()[1537] = byte(0xef);

  ASSERT_EQ(Status::DATA_LOSS, kvs_.Init());

  auto stats = kvs_.GetStorageStats();
  EXPECT_EQ(64u, stats.in_use_bytes);
  EXPECT_EQ(4 * 512u - 64u, stats.reclaimable_bytes);
  EXPECT_EQ(0u, stats.writable_bytes);
}

TEST_F(KvsErrorHandling, Init_CorruptKey_RevertsToPreviousVersion) {
  constexpr auto kVersion7 =
      MakeValidEntry(kMagic, 7, "my_key", ByteStr("version 7"));
  constexpr auto kVersion8 =
      MakeValidEntry(kMagic, 8, "my_key", ByteStr("version 8"));

  InitFlashTo(AsBytes(kVersion7, kVersion8));

  // Corrupt a byte of entry version 8 (addresses 32-63).
  flash_.buffer()[34] = byte(0xef);

  ASSERT_EQ(Status::DATA_LOSS, kvs_.Init());

  char buffer[64] = {};

  EXPECT_EQ(1u, kvs_.size());

  auto result = kvs_.Get("my_key", as_writable_bytes(span(buffer)));
  EXPECT_EQ(Status::OK, result.status());
  EXPECT_EQ(sizeof("version 7") - 1, result.size());
  EXPECT_STREQ("version 7", buffer);

  EXPECT_EQ(32u, kvs_.GetStorageStats().in_use_bytes);
}

// The Put_WriteFailure_EntryNotAddedButBytesMarkedWritten test is run with both
// the KvsErrorRecovery and KvsErrorHandling test fixtures (different KVS
// configurations).
TEST_F(KvsErrorHandling, Put_WriteFailure_EntryNotAddedButBytesMarkedWritten) {
  ASSERT_EQ(Status::OK, kvs_.Init());
  flash_.InjectWriteError(FlashError::Unconditional(Status::UNAVAILABLE, 1));

  EXPECT_EQ(Status::UNAVAILABLE, kvs_.Put("key1", ByteStr("value1")));

  EXPECT_EQ(Status::NOT_FOUND, kvs_.Get("key1", span<byte>()).status());
  ASSERT_TRUE(kvs_.empty());

  auto stats = kvs_.GetStorageStats();
  EXPECT_EQ(stats.in_use_bytes, 0u);
  EXPECT_EQ(stats.reclaimable_bytes, 512u);
  EXPECT_EQ(stats.writable_bytes, 512u * 2);

  // The bytes were marked used, so a new key should not overlap with the bytes
  // from the failed Put.
  EXPECT_EQ(Status::OK, kvs_.Put("key1", ByteStr("value1")));

  stats = kvs_.GetStorageStats();
  EXPECT_EQ(stats.in_use_bytes, (32u * kvs_.redundancy()));
  EXPECT_EQ(stats.reclaimable_bytes, 512u);
  EXPECT_EQ(stats.writable_bytes, 512u * 2 - (32 * kvs_.redundancy()));
}

class KvsErrorRecovery : public ::testing::Test {
 protected:
  KvsErrorRecovery()
      : flash_(internal::Entry::kMinAlignmentBytes),
        partition_(&flash_),
        kvs_(&partition_,
             {.magic = kMagic, .checksum = &checksum},
             kRecoveryNoGcOptions) {}

  void InitFlashTo(span<const byte> contents) {
    partition_.Erase();
    std::memcpy(flash_.buffer().data(), contents.data(), contents.size());
  }

  FakeFlashBuffer<512, 4> flash_;
  FlashPartition partition_;
  KeyValueStoreBuffer<kMaxEntries, kMaxUsableSectors> kvs_;
};

TEST_F(KvsErrorRecovery, Init_Ok) {
  InitFlashTo(AsBytes(kEntry1, kEntry2));

  EXPECT_EQ(Status::OK, kvs_.Init());
  byte buffer[64];
  EXPECT_EQ(Status::OK, kvs_.Get("key1", buffer).status());
  EXPECT_EQ(Status::OK, kvs_.Get("k2", buffer).status());
}

TEST_F(KvsErrorRecovery, Init_DuplicateEntries_RecoversDuringInit) {
  InitFlashTo(AsBytes(kEntry1, kEntry1));

  EXPECT_EQ(Status::OK, kvs_.Init());
  auto stats = kvs_.GetStorageStats();
  EXPECT_EQ(stats.corrupt_sectors_recovered, 1u);

  byte buffer[64];
  EXPECT_EQ(Status::OK, kvs_.Get("key1", buffer).status());
  EXPECT_EQ(Status::NOT_FOUND, kvs_.Get("k2", buffer).status());
}

TEST_F(KvsErrorRecovery, Init_CorruptEntry_FindsSubsequentValidEntry) {
  // Corrupt each byte in the first entry once.
  for (size_t i = 0; i < kEntry1.size(); ++i) {
    InitFlashTo(AsBytes(kEntry1, kEntry2));
    flash_.buffer()[i] = byte(int(flash_.buffer()[i]) + 1);

    ASSERT_EQ(Status::OK, kvs_.Init());
    byte buffer[64];
    ASSERT_EQ(Status::NOT_FOUND, kvs_.Get("key1", buffer).status());
    ASSERT_EQ(Status::OK, kvs_.Get("k2", buffer).status());

    auto stats = kvs_.GetStorageStats();
    // One valid entry.
    ASSERT_EQ(32u, stats.in_use_bytes);
    // The sector with corruption should have been recovered.
    ASSERT_EQ(0u, stats.reclaimable_bytes);
    ASSERT_EQ(i + 1u, stats.corrupt_sectors_recovered);
  }
}

TEST_F(KvsErrorRecovery, Init_CorruptEntry_CorrectlyAccountsForSectorSize) {
  InitFlashTo(AsBytes(kEntry1, kEntry2, kEntry3, kEntry4));

  // Corrupt the first and third entries.
  flash_.buffer()[9] = byte(0xef);
  flash_.buffer()[67] = byte(0xef);

  ASSERT_EQ(Status::OK, kvs_.Init());

  EXPECT_EQ(2u, kvs_.size());

  byte buffer[64];
  EXPECT_EQ(Status::NOT_FOUND, kvs_.Get("key1", buffer).status());
  EXPECT_EQ(Status::OK, kvs_.Get("k2", buffer).status());
  EXPECT_EQ(Status::NOT_FOUND, kvs_.Get("k3y", buffer).status());
  EXPECT_EQ(Status::OK, kvs_.Get("4k", buffer).status());

  auto stats = kvs_.GetStorageStats();
  ASSERT_EQ(64u, stats.in_use_bytes);
  ASSERT_EQ(0u, stats.reclaimable_bytes);
  ASSERT_EQ(1472u, stats.writable_bytes);
  ASSERT_EQ(1u, stats.corrupt_sectors_recovered);
}

TEST_F(KvsErrorRecovery, Init_ReadError_InitializedWithSingleEntryError) {
  InitFlashTo(AsBytes(kEntry1, kEntry2));

  flash_.InjectReadError(
      FlashError::InRange(Status::UNAUTHENTICATED, kEntry1.size()));

  EXPECT_EQ(Status::OK, kvs_.Init());
  EXPECT_TRUE(kvs_.initialized());
  auto stats = kvs_.GetStorageStats();
  ASSERT_EQ(32u, stats.in_use_bytes);
  ASSERT_EQ(0u, stats.reclaimable_bytes);
  ASSERT_EQ(3 * 512u - 32u, stats.writable_bytes);
  ASSERT_EQ(1u, stats.corrupt_sectors_recovered);
  ASSERT_EQ(0u, stats.missing_redundant_entries_recovered);
}

TEST_F(KvsErrorRecovery, Init_CorruptSectors_ShouldBeUnwritable) {
  InitFlashTo(AsBytes(kEntry1, kEntry2));

  // Corrupt 3 of the 4 512-byte flash sectors. Corrupt sectors should be
  // recovered via garbage collection.
  flash_.buffer()[1] = byte(0xef);
  flash_.buffer()[513] = byte(0xef);
  flash_.buffer()[1025] = byte(0xef);

  ASSERT_EQ(Status::OK, kvs_.Init());
  EXPECT_EQ(Status::OK, kvs_.Put("hello", ByteStr("world")));
  EXPECT_EQ(Status::OK, kvs_.Put("a", ByteStr("b")));

  // Existing valid entries should still be readable.
  EXPECT_EQ(3u, kvs_.size());
  byte buffer[64];
  EXPECT_EQ(Status::NOT_FOUND, kvs_.Get("key1", buffer).status());
  EXPECT_EQ(Status::OK, kvs_.Get("k2", buffer).status());

  auto stats = kvs_.GetStorageStats();
  EXPECT_EQ(96u, stats.in_use_bytes);
  EXPECT_EQ(0u, stats.reclaimable_bytes);
  EXPECT_EQ(1440u, stats.writable_bytes);
  EXPECT_EQ(3u, stats.corrupt_sectors_recovered);
}

TEST_F(KvsErrorRecovery, Init_CorruptSectors_ShouldRecoverOne) {
  InitFlashTo(AsBytes(kEntry1, kEntry2));

  // Corrupt all of the 4 512-byte flash sectors. Leave the pre-init entries
  // intact. As part of recovery all corrupt sectors should get garbage
  // collected.
  flash_.buffer()[64] = byte(0xef);
  flash_.buffer()[513] = byte(0xef);
  flash_.buffer()[1025] = byte(0xef);
  flash_.buffer()[1537] = byte(0xef);

  ASSERT_EQ(Status::OK, kvs_.Init());

  auto stats = kvs_.GetStorageStats();
  EXPECT_EQ(64u, stats.in_use_bytes);
  EXPECT_EQ(0u, stats.reclaimable_bytes);
  EXPECT_EQ(3 * 512u - 64u, stats.writable_bytes);
  EXPECT_EQ(4u, stats.corrupt_sectors_recovered);
}

TEST_F(KvsErrorRecovery, Init_CorruptKey_RevertsToPreviousVersion) {
  constexpr auto kVersion7 =
      MakeValidEntry(kMagic, 7, "my_key", ByteStr("version 7"));
  constexpr auto kVersion8 =
      MakeValidEntry(kMagic, 8, "my_key", ByteStr("version 8"));

  InitFlashTo(AsBytes(kVersion7, kVersion8));

  // Corrupt a byte of entry version 8 (addresses 32-63).
  flash_.buffer()[34] = byte(0xef);

  ASSERT_EQ(Status::OK, kvs_.Init());

  char buffer[64] = {};

  EXPECT_EQ(1u, kvs_.size());

  auto result = kvs_.Get("my_key", as_writable_bytes(span(buffer)));
  EXPECT_EQ(Status::OK, result.status());
  EXPECT_EQ(sizeof("version 7") - 1, result.size());
  EXPECT_STREQ("version 7", buffer);

  EXPECT_EQ(32u, kvs_.GetStorageStats().in_use_bytes);
}

// The Put_WriteFailure_EntryNotAddedButBytesMarkedWritten test is run with both
// the KvsErrorRecovery and KvsErrorHandling test fixtures (different KVS
// configurations).
TEST_F(KvsErrorRecovery, Put_WriteFailure_EntryNotAddedButBytesMarkedWritten) {
  ASSERT_EQ(Status::OK, kvs_.Init());
  flash_.InjectWriteError(FlashError::Unconditional(Status::UNAVAILABLE, 1));

  EXPECT_EQ(Status::UNAVAILABLE, kvs_.Put("key1", ByteStr("value1")));
  EXPECT_EQ(true, kvs_.error_detected());

  EXPECT_EQ(Status::NOT_FOUND, kvs_.Get("key1", span<byte>()).status());
  ASSERT_TRUE(kvs_.empty());

  auto stats = kvs_.GetStorageStats();
  EXPECT_EQ(stats.in_use_bytes, 0u);
  EXPECT_EQ(stats.reclaimable_bytes, 512u);
  EXPECT_EQ(stats.writable_bytes, 512u * 2);
  EXPECT_EQ(stats.corrupt_sectors_recovered, 0u);
  EXPECT_EQ(stats.missing_redundant_entries_recovered, 0u);

  // The bytes were marked used, so a new key should not overlap with the bytes
  // from the failed Put.
  EXPECT_EQ(Status::OK, kvs_.Put("key1", ByteStr("value1")));

  stats = kvs_.GetStorageStats();
  EXPECT_EQ(stats.in_use_bytes, (32u * kvs_.redundancy()));
  EXPECT_EQ(stats.reclaimable_bytes, 512u);
  EXPECT_EQ(stats.writable_bytes, 512u * 2 - (32 * kvs_.redundancy()));
  EXPECT_EQ(stats.corrupt_sectors_recovered, 0u);
  EXPECT_EQ(stats.missing_redundant_entries_recovered, 0u);
}

constexpr uint32_t kAltMagic = 0xbadD00D;

constexpr uint32_t AltChecksum(span<const byte> data, uint32_t state) {
  for (byte b : data) {
    state = (state << 8) | uint32_t(byte(state >> 24) ^ b);
  }
  return state;
}

ChecksumFunction<uint32_t> alt_checksum(AltChecksum);

constexpr auto kAltEntry =
    MakeValidEntry<AltChecksum>(kAltMagic, 32, "A Key", ByteStr("XD"));

constexpr uint32_t NoChecksum(span<const byte>, uint32_t) { return 0; }
constexpr uint32_t kNoChecksumMagic = 0x6000061e;

constexpr auto kNoChecksumEntry =
    MakeValidEntry<NoChecksum>(kNoChecksumMagic, 64, "kee", ByteStr("O_o"));

class InitializedMultiMagicKvs : public ::testing::Test {
 protected:
  static constexpr auto kInitialContents =
      AsBytes(kNoChecksumEntry, kEntry1, kAltEntry, kEntry2, kEntry3);

  InitializedMultiMagicKvs()
      : flash_(internal::Entry::kMinAlignmentBytes),
        partition_(&flash_),
        kvs_(&partition_,
             {{
                 {.magic = kMagic, .checksum = &checksum},
                 {.magic = kAltMagic, .checksum = &alt_checksum},
                 {.magic = kNoChecksumMagic, .checksum = nullptr},
             }},
             kRecoveryNoGcOptions) {
    partition_.Erase();
    std::memcpy(flash_.buffer().data(),
                kInitialContents.data(),
                kInitialContents.size());

    EXPECT_EQ(Status::OK, kvs_.Init());
  }

  FakeFlashBuffer<512, 4, 3> flash_;
  FlashPartition partition_;
  KeyValueStoreBuffer<kMaxEntries, kMaxUsableSectors, 2, 3> kvs_;
};

#define ASSERT_CONTAINS_ENTRY(key, str_value)                          \
  do {                                                                 \
    char val[sizeof(str_value)] = {};                                  \
    StatusWithSize stat = kvs_.Get(key, as_writable_bytes(span(val))); \
    ASSERT_EQ(Status::OK, stat.status());                              \
    ASSERT_EQ(sizeof(str_value) - 1, stat.size());                     \
    ASSERT_STREQ(str_value, val);                                      \
  } while (0)

TEST_F(InitializedMultiMagicKvs, AllEntriesArePresent) {
  ASSERT_CONTAINS_ENTRY("key1", "value1");
  ASSERT_CONTAINS_ENTRY("k2", "value2");
  ASSERT_CONTAINS_ENTRY("k3y", "value3");
  ASSERT_CONTAINS_ENTRY("A Key", "XD");
  ASSERT_CONTAINS_ENTRY("kee", "O_o");
}

TEST_F(InitializedMultiMagicKvs, RecoversLossOfFirstSector) {
  auto stats = kvs_.GetStorageStats();
  EXPECT_EQ(stats.in_use_bytes, (160u * kvs_.redundancy()));
  EXPECT_EQ(stats.reclaimable_bytes, 0u);
  EXPECT_EQ(stats.writable_bytes, 512u * 3 - (160 * kvs_.redundancy()));
  EXPECT_EQ(stats.corrupt_sectors_recovered, 0u);
  EXPECT_EQ(stats.missing_redundant_entries_recovered, 5u);

  EXPECT_EQ(Status::OK, partition_.Erase(0, 1));

  ASSERT_CONTAINS_ENTRY("key1", "value1");
  ASSERT_CONTAINS_ENTRY("k2", "value2");
  ASSERT_CONTAINS_ENTRY("k3y", "value3");
  ASSERT_CONTAINS_ENTRY("A Key", "XD");
  ASSERT_CONTAINS_ENTRY("kee", "O_o");

  EXPECT_EQ(true, kvs_.error_detected());

  stats = kvs_.GetStorageStats();
  EXPECT_EQ(stats.in_use_bytes, (160u * kvs_.redundancy()));
  EXPECT_EQ(stats.reclaimable_bytes, 352u);
  EXPECT_EQ(stats.writable_bytes, 512u * 2 - (160 * (kvs_.redundancy() - 1)));
  EXPECT_EQ(stats.corrupt_sectors_recovered, 0u);
  EXPECT_EQ(stats.missing_redundant_entries_recovered, 5u);

  EXPECT_EQ(Status::OK, kvs_.FullMaintenance());
  stats = kvs_.GetStorageStats();
  EXPECT_EQ(stats.in_use_bytes, (160u * kvs_.redundancy()));
  EXPECT_EQ(stats.reclaimable_bytes, 0u);
  EXPECT_EQ(stats.writable_bytes, 512u * 3 - (160 * kvs_.redundancy()));
  EXPECT_EQ(stats.corrupt_sectors_recovered, 0u);
  EXPECT_EQ(stats.missing_redundant_entries_recovered, 10u);
}

TEST_F(InitializedMultiMagicKvs, RecoversLossOfSecondSector) {
  auto stats = kvs_.GetStorageStats();
  EXPECT_EQ(stats.in_use_bytes, (160u * kvs_.redundancy()));
  EXPECT_EQ(stats.reclaimable_bytes, 0u);
  EXPECT_EQ(stats.writable_bytes, 512u * 3 - (160 * kvs_.redundancy()));
  EXPECT_EQ(stats.corrupt_sectors_recovered, 0u);
  EXPECT_EQ(stats.missing_redundant_entries_recovered, 5u);

  EXPECT_EQ(Status::OK, partition_.Erase(partition_.sector_size_bytes(), 1));

  ASSERT_CONTAINS_ENTRY("key1", "value1");
  ASSERT_CONTAINS_ENTRY("k2", "value2");
  ASSERT_CONTAINS_ENTRY("k3y", "value3");
  ASSERT_CONTAINS_ENTRY("A Key", "XD");
  ASSERT_CONTAINS_ENTRY("kee", "O_o");

  EXPECT_EQ(false, kvs_.error_detected());

  EXPECT_EQ(false, kvs_.Init());
  stats = kvs_.GetStorageStats();
  EXPECT_EQ(stats.in_use_bytes, (160u * kvs_.redundancy()));
  EXPECT_EQ(stats.reclaimable_bytes, 0u);
  EXPECT_EQ(stats.writable_bytes, 512u * 3 - (160 * kvs_.redundancy()));
  EXPECT_EQ(stats.corrupt_sectors_recovered, 0u);
  EXPECT_EQ(stats.missing_redundant_entries_recovered, 10u);
}

TEST_F(InitializedMultiMagicKvs, SingleReadErrors) {
  // Inject 2 read errors, so the first read attempt fully fails.
  flash_.InjectReadError(FlashError::Unconditional(Status::INTERNAL, 2));

  flash_.InjectReadError(FlashError::Unconditional(Status::INTERNAL, 1, 7));

  ASSERT_CONTAINS_ENTRY("key1", "value1");
  ASSERT_CONTAINS_ENTRY("k2", "value2");
  ASSERT_CONTAINS_ENTRY("k3y", "value3");
  ASSERT_CONTAINS_ENTRY("A Key", "XD");
  ASSERT_CONTAINS_ENTRY("kee", "O_o");

  EXPECT_EQ(true, kvs_.error_detected());

  auto stats = kvs_.GetStorageStats();
  EXPECT_EQ(stats.in_use_bytes, (160u * kvs_.redundancy()));
  EXPECT_EQ(stats.reclaimable_bytes, 352u);
  EXPECT_EQ(stats.writable_bytes, 512u * 2 - (160 * (kvs_.redundancy() - 1)));
  EXPECT_EQ(stats.corrupt_sectors_recovered, 0u);
  EXPECT_EQ(stats.missing_redundant_entries_recovered, 5u);
}

TEST_F(InitializedMultiMagicKvs, SingleWriteError) {
  flash_.InjectWriteError(FlashError::Unconditional(Status::INTERNAL, 1, 1));

  EXPECT_EQ(Status::INTERNAL, kvs_.Put("new key", ByteStr("abcd?")));

  EXPECT_EQ(true, kvs_.error_detected());

  auto stats = kvs_.GetStorageStats();
  EXPECT_EQ(stats.in_use_bytes, 32 + (160u * kvs_.redundancy()));
  EXPECT_EQ(stats.reclaimable_bytes, 352u);
  EXPECT_EQ(stats.writable_bytes,
            512u * 2 - 32 - (160 * (kvs_.redundancy() - 1)));
  EXPECT_EQ(stats.corrupt_sectors_recovered, 0u);
  EXPECT_EQ(stats.missing_redundant_entries_recovered, 5u);

  char val[20] = {};
  EXPECT_EQ(Status::OK,
            kvs_.Get("new key", as_writable_bytes(span(val))).status());

  EXPECT_EQ(Status::OK, kvs_.FullMaintenance());
  stats = kvs_.GetStorageStats();
  EXPECT_EQ(stats.in_use_bytes, (192u * kvs_.redundancy()));
  EXPECT_EQ(stats.reclaimable_bytes, 0u);
  EXPECT_EQ(stats.writable_bytes, 512u * 3 - (192 * kvs_.redundancy()));
  EXPECT_EQ(stats.corrupt_sectors_recovered, 0u);
  EXPECT_EQ(stats.missing_redundant_entries_recovered, 5u);

  EXPECT_EQ(Status::OK,
            kvs_.Get("new key", as_writable_bytes(span(val))).status());
}

TEST_F(InitializedMultiMagicKvs, DataLossAfterLosingBothCopies) {
  EXPECT_EQ(Status::OK, partition_.Erase(0, 2));

  char val[20] = {};
  EXPECT_EQ(Status::DATA_LOSS,
            kvs_.Get("key1", as_writable_bytes(span(val))).status());
  EXPECT_EQ(Status::DATA_LOSS,
            kvs_.Get("k2", as_writable_bytes(span(val))).status());
  EXPECT_EQ(Status::DATA_LOSS,
            kvs_.Get("k3y", as_writable_bytes(span(val))).status());
  EXPECT_EQ(Status::DATA_LOSS,
            kvs_.Get("A Key", as_writable_bytes(span(val))).status());
  EXPECT_EQ(Status::DATA_LOSS,
            kvs_.Get("kee", as_writable_bytes(span(val))).status());

  EXPECT_EQ(true, kvs_.error_detected());

  auto stats = kvs_.GetStorageStats();
  EXPECT_EQ(stats.in_use_bytes, (160u * kvs_.redundancy()));
  EXPECT_EQ(stats.reclaimable_bytes, 2 * 352u);
  EXPECT_EQ(stats.writable_bytes, 512u);
  EXPECT_EQ(stats.corrupt_sectors_recovered, 0u);
  EXPECT_EQ(stats.missing_redundant_entries_recovered, 5u);
}

class RedundantKvsInitializedSingleCopyData : public ::testing::Test {
 protected:
  static constexpr auto kInitialContents =
      AsBytes(kEntry1, kEntry2, kEntry3, kEntry4);

  RedundantKvsInitializedSingleCopyData()
      : flash_(internal::Entry::kMinAlignmentBytes),
        partition_(&flash_),
        kvs_(&partition_,
             {.magic = kMagic, .checksum = &checksum},
             kRecoveryLazyGcOptions) {
    partition_.Erase();
    std::memcpy(flash_.buffer().data(),
                kInitialContents.data(),
                kInitialContents.size());

    EXPECT_EQ(Status::OK, kvs_.Init());
  }

  FakeFlashBuffer<512, 4, 3> flash_;
  FlashPartition partition_;
  KeyValueStoreBuffer<kMaxEntries, kMaxUsableSectors, 2> kvs_;
};

TEST_F(RedundantKvsInitializedSingleCopyData, WriteAfterDataLoss) {
  EXPECT_EQ(Status::OK, partition_.Erase(0, 4));

  char val[20] = {};
  EXPECT_EQ(Status::DATA_LOSS,
            kvs_.Get("key1", as_writable_bytes(span(val))).status());
  EXPECT_EQ(Status::DATA_LOSS,
            kvs_.Get("k2", as_writable_bytes(span(val))).status());
  EXPECT_EQ(Status::DATA_LOSS,
            kvs_.Get("k3y", as_writable_bytes(span(val))).status());
  EXPECT_EQ(Status::DATA_LOSS,
            kvs_.Get("4k", as_writable_bytes(span(val))).status());

  EXPECT_EQ(true, kvs_.error_detected());

  auto stats = kvs_.GetStorageStats();
  EXPECT_EQ(stats.in_use_bytes, (128u * kvs_.redundancy()));
  EXPECT_EQ(stats.reclaimable_bytes, 2 * 384u);
  EXPECT_EQ(stats.writable_bytes, 512u);
  EXPECT_EQ(stats.corrupt_sectors_recovered, 0u);
  EXPECT_EQ(stats.missing_redundant_entries_recovered, 4u);

  ASSERT_EQ(Status::DATA_LOSS, kvs_.Put("key1", 1000));

  EXPECT_EQ(Status::OK, kvs_.FullMaintenance());
  stats = kvs_.GetStorageStats();
  EXPECT_EQ(stats.in_use_bytes, 0u);
  EXPECT_EQ(stats.reclaimable_bytes, 0u);
  EXPECT_EQ(stats.writable_bytes, 3 * 512u);
  EXPECT_EQ(stats.corrupt_sectors_recovered, 0u);
  EXPECT_EQ(stats.missing_redundant_entries_recovered, 4u);
}

TEST_F(RedundantKvsInitializedSingleCopyData,
       TwoSectorsCorruptWithGoodEntries) {
  ASSERT_CONTAINS_ENTRY("key1", "value1");
  ASSERT_CONTAINS_ENTRY("k2", "value2");
  ASSERT_CONTAINS_ENTRY("k3y", "value3");
  ASSERT_CONTAINS_ENTRY("4k", "value4");

  EXPECT_EQ(false, kvs_.error_detected());

  auto stats = kvs_.GetStorageStats();
  EXPECT_EQ(stats.in_use_bytes, (128u * kvs_.redundancy()));
  EXPECT_EQ(stats.reclaimable_bytes, 0u);
  EXPECT_EQ(stats.writable_bytes, 3 * 512u - (128u * kvs_.redundancy()));
  EXPECT_EQ(stats.corrupt_sectors_recovered, 0u);
  EXPECT_EQ(stats.missing_redundant_entries_recovered, 4u);

  // Corrupt all the keys, alternating which copy gets corrupted.
  flash_.buffer()[0x10] = byte(0xef);
  flash_.buffer()[0x230] = byte(0xef);
  flash_.buffer()[0x50] = byte(0xef);
  flash_.buffer()[0x270] = byte(0xef);

  ASSERT_CONTAINS_ENTRY("key1", "value1");
  ASSERT_CONTAINS_ENTRY("k2", "value2");
  ASSERT_CONTAINS_ENTRY("k3y", "value3");
  ASSERT_CONTAINS_ENTRY("4k", "value4");

  EXPECT_EQ(Status::OK, kvs_.FullMaintenance());
  stats = kvs_.GetStorageStats();
  EXPECT_EQ(stats.in_use_bytes, (128u * kvs_.redundancy()));
  EXPECT_EQ(stats.reclaimable_bytes, 0u);
  EXPECT_EQ(stats.writable_bytes, 3 * 512u - (128u * kvs_.redundancy()));
  EXPECT_EQ(stats.corrupt_sectors_recovered, 2u);
  EXPECT_EQ(stats.missing_redundant_entries_recovered, 8u);
}

TEST_F(InitializedMultiMagicKvs, PutNewEntry_UsesFirstFormat) {
  EXPECT_EQ(Status::OK, kvs_.Put("new key", ByteStr("abcd?")));

  constexpr auto kNewEntry =
      MakeValidEntry(kMagic, 65, "new key", ByteStr("abcd?"));
  EXPECT_EQ(0,
            std::memcmp(kNewEntry.data(),
                        flash_.buffer().data() + kInitialContents.size(),
                        kNewEntry.size()));
  ASSERT_CONTAINS_ENTRY("new key", "abcd?");
}

TEST_F(InitializedMultiMagicKvs, PutExistingEntry_UsesFirstFormat) {
  EXPECT_EQ(Status::OK, kvs_.Put("A Key", ByteStr("New value!")));

  constexpr auto kNewEntry =
      MakeValidEntry(kMagic, 65, "A Key", ByteStr("New value!"));
  EXPECT_EQ(0,
            std::memcmp(kNewEntry.data(),
                        flash_.buffer().data() + kInitialContents.size(),
                        kNewEntry.size()));
  ASSERT_CONTAINS_ENTRY("A Key", "New value!");
}

}  // namespace
}  // namespace pw::kvs
