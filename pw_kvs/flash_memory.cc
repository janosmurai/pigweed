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

#define PW_LOG_MODULE_NAME "KVS"
#define PW_LOG_LEVEL PW_KVS_LOG_LEVEL

#include "pw_kvs/flash_memory.h"

#include <algorithm>
#include <cinttypes>
#include <cstring>

#include "pw_assert/assert.h"
#include "pw_kvs_private/config.h"
#include "pw_log/log.h"
#include "pw_status/status_with_size.h"
#include "pw_status/try.h"

namespace pw::kvs {

using std::byte;

StatusWithSize FlashPartition::Output::DoWrite(std::span<const byte> data) {
  PW_TRY_WITH_SIZE(flash_.Write(address_, data));
  address_ += data.size();
  return StatusWithSize(data.size());
}

StatusWithSize FlashPartition::Input::DoRead(std::span<byte> data) {
  StatusWithSize result = flash_.Read(address_, data);
  address_ += result.size();
  return result;
}

FlashPartition::FlashPartition(
    FlashMemory* flash,
    uint32_t start_sector_index,
    uint32_t sector_count,
    uint32_t alignment_bytes,  // Defaults to flash alignment
    PartitionPermission permission)

    : flash_(*flash),
      start_sector_index_(start_sector_index),
      sector_count_(sector_count),
      alignment_bytes_(
          alignment_bytes == 0
              ? flash_.alignment_bytes()
              : std::max(alignment_bytes, uint32_t(flash_.alignment_bytes()))),
      permission_(permission) {
  uint32_t misalignment = (alignment_bytes_ % flash_.alignment_bytes());
  PW_DCHECK_UINT_EQ(misalignment,
                    0,
                    "Flash partition alignmentmust be a multiple of the flash "
                    "memory alignment");
}

Status FlashPartition::Erase(Address address, size_t num_sectors) {
  if (permission_ == PartitionPermission::kReadOnly) {
    return Status::PermissionDenied();
  }

  PW_TRY(CheckBounds(address, num_sectors * sector_size_bytes()));
  const size_t address_sector_offset = address % sector_size_bytes();
  PW_CHECK_UINT_EQ(address_sector_offset, 0u);

  return flash_.Erase(PartitionToFlashAddress(address), num_sectors);
}

StatusWithSize FlashPartition::Read(Address address, std::span<byte> output) {
  PW_TRY_WITH_SIZE(CheckBounds(address, output.size()));
  return flash_.Read(PartitionToFlashAddress(address), output);
}

StatusWithSize FlashPartition::Write(Address address,
                                     std::span<const byte> data) {
  if (permission_ == PartitionPermission::kReadOnly) {
    return StatusWithSize::PermissionDenied();
  }
  PW_TRY_WITH_SIZE(CheckBounds(address, data.size()));
  const size_t address_alignment_offset = address % alignment_bytes();
  PW_CHECK_UINT_EQ(address_alignment_offset, 0u);
  const size_t size_alignment_offset = data.size() % alignment_bytes();
  PW_CHECK_UINT_EQ(size_alignment_offset, 0u);
  return flash_.Write(PartitionToFlashAddress(address), data);
}

Status FlashPartition::IsRegionErased(Address source_flash_address,
                                      size_t length,
                                      bool* is_erased) {
  // Relying on Read() to check address and len arguments.
  if (is_erased == nullptr) {
    return Status::InvalidArgument();
  }

  // TODO(pwbug/214): Currently using a single flash alignment to do both the
  // read and write. The allowable flash read length may be less than what write
  // needs (possibly by a bunch), resulting in buffer and erased_pattern_buffer
  // being bigger than they need to be.
  const size_t alignment = alignment_bytes();
  if (alignment > kMaxFlashAlignment || kMaxFlashAlignment % alignment ||
      length % alignment) {
    return Status::InvalidArgument();
  }

  byte buffer[kMaxFlashAlignment];
  const byte erased_byte = flash_.erased_memory_content();
  size_t offset = 0;
  *is_erased = false;
  while (length > 0u) {
    // Check earlier that length is aligned, no need to round up
    size_t read_size = std::min(sizeof(buffer), length);
    PW_TRY(Read(source_flash_address + offset, read_size, buffer).status());

    for (byte b : std::span(buffer, read_size)) {
      if (b != erased_byte) {
        // Detected memory chunk is not entirely erased
        return Status::Ok();
      }
    }

    offset += read_size;
    length -= read_size;
  }
  *is_erased = true;
  return Status::Ok();
}

bool FlashPartition::AppearsErased(std::span<const byte> data) const {
  byte erased_content = flash_.erased_memory_content();
  for (byte b : data) {
    if (b != erased_content) {
      return false;
    }
  }
  return true;
}

Status FlashPartition::CheckBounds(Address address, size_t length) const {
  if (address + length > size_bytes()) {
    PW_LOG_ERROR(
        "Attempted out-of-bound flash memory access (address: %u length: %u)",
        unsigned(address),
        unsigned(length));
    return Status::OutOfRange();
  }
  return Status::Ok();
}

}  // namespace pw::kvs
