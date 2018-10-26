//
// Copyright (C) 2015 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "update_engine/boot_control_android.h"

#include <memory>
#include <utility>
#include <vector>

#include <base/bind.h>
#include <base/logging.h>
#include <base/strings/string_util.h>
#include <bootloader_message/bootloader_message.h>
#include <brillo/message_loops/message_loop.h>
#include <fs_mgr.h>

#include "update_engine/common/utils.h"
#include "update_engine/dynamic_partition_control_android.h"

using std::string;

using android::dm::DmDeviceState;
using android::fs_mgr::Partition;
using android::hardware::hidl_string;
using android::hardware::Return;
using android::hardware::boot::V1_0::BoolResult;
using android::hardware::boot::V1_0::CommandResult;
using android::hardware::boot::V1_0::IBootControl;
using Slot = chromeos_update_engine::BootControlInterface::Slot;
using PartitionMetadata =
    chromeos_update_engine::BootControlInterface::PartitionMetadata;

namespace {

auto StoreResultCallback(CommandResult* dest) {
  return [dest](const CommandResult& result) { *dest = result; };
}
}  // namespace

namespace chromeos_update_engine {

namespace boot_control {

// Factory defined in boot_control.h.
std::unique_ptr<BootControlInterface> CreateBootControl() {
  auto boot_control = std::make_unique<BootControlAndroid>();
  if (!boot_control->Init()) {
    return nullptr;
  }
  return std::move(boot_control);
}

}  // namespace boot_control

bool BootControlAndroid::Init() {
  module_ = IBootControl::getService();
  if (module_ == nullptr) {
    LOG(ERROR) << "Error getting bootctrl HIDL module.";
    return false;
  }

  LOG(INFO) << "Loaded boot control hidl hal.";

  dynamic_control_ = std::make_unique<DynamicPartitionControlAndroid>();

  return true;
}

void BootControlAndroid::Cleanup() {
  dynamic_control_->Cleanup();
}

unsigned int BootControlAndroid::GetNumSlots() const {
  return module_->getNumberSlots();
}

BootControlInterface::Slot BootControlAndroid::GetCurrentSlot() const {
  return module_->getCurrentSlot();
}

bool BootControlAndroid::GetSuffix(Slot slot, string* suffix) const {
  auto store_suffix_cb = [&suffix](hidl_string cb_suffix) {
    *suffix = cb_suffix.c_str();
  };
  Return<void> ret = module_->getSuffix(slot, store_suffix_cb);

  if (!ret.isOk()) {
    LOG(ERROR) << "boot_control impl returned no suffix for slot "
               << SlotName(slot);
    return false;
  }
  return true;
}

bool BootControlAndroid::GetPartitionDevice(const string& partition_name,
                                            Slot slot,
                                            string* device) const {
  string suffix;
  if (!GetSuffix(slot, &suffix)) {
    return false;
  }

  const string target_partition_name = partition_name + suffix;

  // DeltaPerformer calls InitPartitionMetadata before calling
  // InstallPlan::LoadPartitionsFromSlots. After InitPartitionMetadata,
  // the partition must be re-mapped with force_writable == true. Hence,
  // we only need to check device mapper.
  if (dynamic_control_->IsDynamicPartitionsEnabled()) {
    switch (dynamic_control_->GetState(target_partition_name)) {
      case DmDeviceState::ACTIVE:
        if (dynamic_control_->GetDmDevicePathByName(target_partition_name,
                                                    device)) {
          LOG(INFO) << target_partition_name
                    << " is mapped on device mapper: " << *device;
          return true;
        }
        LOG(ERROR) << target_partition_name
                   << " is mapped but path is unknown.";
        return false;

      case DmDeviceState::INVALID:
        // Try static partitions.
        break;

      case DmDeviceState::SUSPENDED:  // fallthrough
      default:
        LOG(ERROR) << target_partition_name
                   << " is mapped on device mapper but state is unknown";
        return false;
    }
  }

  string device_dir_str;
  if (!dynamic_control_->GetDeviceDir(&device_dir_str)) {
    return false;
  }

  base::FilePath path =
      base::FilePath(device_dir_str).Append(target_partition_name);
  if (!dynamic_control_->DeviceExists(path.value())) {
    LOG(ERROR) << "Device file " << path.value() << " does not exist.";
    return false;
  }

  *device = path.value();
  return true;
}

bool BootControlAndroid::IsSlotBootable(Slot slot) const {
  Return<BoolResult> ret = module_->isSlotBootable(slot);
  if (!ret.isOk()) {
    LOG(ERROR) << "Unable to determine if slot " << SlotName(slot)
               << " is bootable: "
               << ret.description();
    return false;
  }
  if (ret == BoolResult::INVALID_SLOT) {
    LOG(ERROR) << "Invalid slot: " << SlotName(slot);
    return false;
  }
  return ret == BoolResult::TRUE;
}

bool BootControlAndroid::MarkSlotUnbootable(Slot slot) {
  CommandResult result;
  auto ret = module_->setSlotAsUnbootable(slot, StoreResultCallback(&result));
  if (!ret.isOk()) {
    LOG(ERROR) << "Unable to call MarkSlotUnbootable for slot "
               << SlotName(slot) << ": "
               << ret.description();
    return false;
  }
  if (!result.success) {
    LOG(ERROR) << "Unable to mark slot " << SlotName(slot)
               << " as unbootable: " << result.errMsg.c_str();
  }
  return result.success;
}

bool BootControlAndroid::SetActiveBootSlot(Slot slot) {
  CommandResult result;
  auto ret = module_->setActiveBootSlot(slot, StoreResultCallback(&result));
  if (!ret.isOk()) {
    LOG(ERROR) << "Unable to call SetActiveBootSlot for slot " << SlotName(slot)
               << ": " << ret.description();
    return false;
  }
  if (!result.success) {
    LOG(ERROR) << "Unable to set the active slot to slot " << SlotName(slot)
               << ": " << result.errMsg.c_str();
  }
  return result.success;
}

bool BootControlAndroid::MarkBootSuccessfulAsync(
    base::Callback<void(bool)> callback) {
  CommandResult result;
  auto ret = module_->markBootSuccessful(StoreResultCallback(&result));
  if (!ret.isOk()) {
    LOG(ERROR) << "Unable to call MarkBootSuccessful: "
               << ret.description();
    return false;
  }
  if (!result.success) {
    LOG(ERROR) << "Unable to mark boot successful: " << result.errMsg.c_str();
  }
  return brillo::MessageLoop::current()->PostTask(
             FROM_HERE, base::Bind(callback, result.success)) !=
         brillo::MessageLoop::kTaskIdNull;
}

namespace {

bool InitPartitionMetadataInternal(
    DynamicPartitionControlInterface* dynamic_control,
    const string& super_device,
    Slot source_slot,
    Slot target_slot,
    const string& target_suffix,
    const PartitionMetadata& partition_metadata) {
  auto builder =
      dynamic_control->LoadMetadataBuilder(super_device, source_slot);
  if (builder == nullptr) {
    // TODO(elsk): allow reconstructing metadata from partition_metadata
    // in recovery sideload.
    LOG(ERROR) << "No metadata at "
               << BootControlInterface::SlotName(source_slot);
    return false;
  }

  std::vector<string> groups = builder->ListGroups();
  for (const auto& group_name : groups) {
    if (base::EndsWith(
            group_name, target_suffix, base::CompareCase::SENSITIVE)) {
      LOG(INFO) << "Removing group " << group_name;
      builder->RemoveGroupAndPartitions(group_name);
    }
  }

  uint64_t total_size = 0;
  for (const auto& group : partition_metadata.groups) {
    total_size += group.size;
  }

  if (total_size > (builder->AllocatableSpace() / 2)) {
    LOG(ERROR)
        << "The maximum size of all groups with suffix " << target_suffix
        << " (" << total_size
        << ") has exceeded half of allocatable space for dynamic partitions "
        << (builder->AllocatableSpace() / 2) << ".";
    return false;
  }

  for (const auto& group : partition_metadata.groups) {
    auto group_name_suffix = group.name + target_suffix;
    if (!builder->AddGroup(group_name_suffix, group.size)) {
      LOG(ERROR) << "Cannot add group " << group_name_suffix << " with size "
                 << group.size;
      return false;
    }
    LOG(INFO) << "Added group " << group_name_suffix << " with size "
              << group.size;

    for (const auto& partition : group.partitions) {
      auto parition_name_suffix = partition.name + target_suffix;
      Partition* p = builder->AddPartition(
          parition_name_suffix, group_name_suffix, LP_PARTITION_ATTR_READONLY);
      if (!p) {
        LOG(ERROR) << "Cannot add partition " << parition_name_suffix
                   << " to group " << group_name_suffix;
        return false;
      }
      if (!builder->ResizePartition(p, partition.size)) {
        LOG(ERROR) << "Cannot resize partition " << parition_name_suffix
                   << " to size " << partition.size << ". Not enough space?";
        return false;
      }
      LOG(INFO) << "Added partition " << parition_name_suffix << " to group "
                << group_name_suffix << " with size " << partition.size;
    }
  }

  return dynamic_control->StoreMetadata(
      super_device, builder.get(), target_slot);
}

// Unmap all partitions, and remap partitions.
bool Remap(DynamicPartitionControlInterface* dynamic_control,
           const string& super_device,
           Slot target_slot,
           const string& target_suffix,
           const PartitionMetadata& partition_metadata) {
  for (const auto& group : partition_metadata.groups) {
    for (const auto& partition : group.partitions) {
      if (!dynamic_control->UnmapPartitionOnDeviceMapper(
              partition.name + target_suffix, true /* wait */)) {
        return false;
      }
      if (partition.size == 0) {
        continue;
      }
      string map_path;
      if (!dynamic_control->MapPartitionOnDeviceMapper(
              super_device,
              partition.name + target_suffix,
              target_slot,
              &map_path)) {
        return false;
      }
    }
  }
  return true;
}

}  // namespace

bool BootControlAndroid::InitPartitionMetadata(
    Slot target_slot, const PartitionMetadata& partition_metadata) {
  if (!dynamic_control_->IsDynamicPartitionsEnabled()) {
    return true;
  }

  string device_dir_str;
  if (!dynamic_control_->GetDeviceDir(&device_dir_str)) {
    return false;
  }
  base::FilePath device_dir(device_dir_str);
  string super_device =
      device_dir.Append(fs_mgr_get_super_partition_name()).value();

  Slot current_slot = GetCurrentSlot();
  if (target_slot == current_slot) {
    LOG(ERROR) << "Cannot call InitPartitionMetadata on current slot.";
    return false;
  }

  string target_suffix;
  if (!GetSuffix(target_slot, &target_suffix)) {
    return false;
  }

  if (!InitPartitionMetadataInternal(dynamic_control_.get(),
                                     super_device,
                                     current_slot,
                                     target_slot,
                                     target_suffix,
                                     partition_metadata)) {
    return false;
  }

  if (!Remap(dynamic_control_.get(),
             super_device,
             target_slot,
             target_suffix,
             partition_metadata)) {
    return false;
  }

  return true;
}

}  // namespace chromeos_update_engine
