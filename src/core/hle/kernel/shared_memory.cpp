// Copyright 2014 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <cstring>
#include "common/logging/log.h"
#include "core/hle/kernel/errors.h"
#include "core/hle/kernel/memory.h"
#include "core/hle/kernel/shared_memory.h"
#include "core/memory.h"

namespace Kernel {

SharedMemory::SharedMemory(KernelSystem& kernel) : Object(kernel), kernel(kernel) {}
SharedMemory::~SharedMemory() {
    for (const auto& interval : holding_memory) {
        kernel.GetMemoryRegion(MemoryRegion::SYSTEM)
            ->Free(interval.lower(), interval.upper() - interval.lower());
    }
    if (base_address != 0 && owner_process != nullptr) {
        owner_process->vm_manager.ChangeMemoryState(base_address, size, MemoryState::Locked,
                                                    VMAPermission::None, MemoryState::Private,
                                                    VMAPermission::ReadWrite);
    }
}

ResultVal<SharedPtr<SharedMemory>> KernelSystem::CreateSharedMemory(
    Process* owner_process, u32 size, MemoryPermission permissions,
    MemoryPermission other_permissions, VAddr address, MemoryRegion region, std::string name) {
    SharedPtr<SharedMemory> shared_memory(new SharedMemory(*this));

    shared_memory->owner_process = owner_process;
    shared_memory->name = std::move(name);
    shared_memory->size = size;
    shared_memory->permissions = permissions;
    shared_memory->other_permissions = other_permissions;

    if (address == 0) {
        // We need to allocate a block from the Linear Heap ourselves.
        // We'll manually allocate some memory from the linear heap in the specified region.
        MemoryRegionInfo* memory_region = GetMemoryRegion(region);
        auto offset = memory_region->LinearAllocate(size);

        ASSERT_MSG(offset, "Not enough space in region to allocate shared memory!");

        std::fill(memory.fcram.data() + *offset, memory.fcram.data() + *offset + size, 0);
        shared_memory->backing_blocks = {{memory.fcram.data() + *offset, size}};
        shared_memory->holding_memory += MemoryRegionInfo::Interval(*offset, *offset + size);
        shared_memory->linear_heap_phys_offset = *offset;

        // Increase the amount of used linear heap memory for the owner process.
        if (shared_memory->owner_process != nullptr) {
            shared_memory->owner_process->memory_used += size;
        }
    } else {
        auto& vm_manager = shared_memory->owner_process->vm_manager;
        // The memory is already available and mapped in the owner process.

        CASCADE_CODE(vm_manager.ChangeMemoryState(address, size, MemoryState::Private,
                                                  VMAPermission::ReadWrite, MemoryState::Locked,
                                                  SharedMemory::ConvertPermissions(permissions)));

        auto backing_blocks = vm_manager.GetBackingBlocksForRange(address, size);
        ASSERT(backing_blocks.Succeeded()); // should success after verifying memory state above
        shared_memory->backing_blocks = std::move(backing_blocks).Unwrap();
    }

    shared_memory->base_address = address;
    return MakeResult(shared_memory);
}

SharedPtr<SharedMemory> KernelSystem::CreateSharedMemoryForApplet(
    u32 offset, u32 size, MemoryPermission permissions, MemoryPermission other_permissions,
    std::string name) {
    SharedPtr<SharedMemory> shared_memory(new SharedMemory(*this));

    // Allocate memory in heap
    MemoryRegionInfo* memory_region = GetMemoryRegion(MemoryRegion::SYSTEM);
    auto backing_blocks = memory_region->HeapAllocate(size);
    ASSERT_MSG(!backing_blocks.empty(), "Not enough space in region to allocate shared memory!");
    shared_memory->holding_memory = backing_blocks;
    shared_memory->owner_process = nullptr;
    shared_memory->name = std::move(name);
    shared_memory->size = size;
    shared_memory->permissions = permissions;
    shared_memory->other_permissions = other_permissions;
    for (const auto& interval : backing_blocks) {
        shared_memory->backing_blocks.push_back(
            {memory.fcram.data() + interval.lower(), interval.upper() - interval.lower()});
        std::fill(memory.fcram.data() + interval.lower(), memory.fcram.data() + interval.upper(),
                  0);
    }
    shared_memory->base_address = Memory::HEAP_VADDR + offset;

    return shared_memory;
}

ResultCode SharedMemory::Map(Process& target_process, VAddr address, MemoryPermission permissions,
                             MemoryPermission other_permissions) {

    MemoryPermission own_other_permissions =
        &target_process == owner_process ? this->permissions : this->other_permissions;

    // Automatically allocated memory blocks can only be mapped with other_permissions = DontCare
    if (base_address == 0 && other_permissions != MemoryPermission::DontCare) {
        return ERR_INVALID_COMBINATION;
    }

    // Error out if the requested permissions don't match what the creator process allows.
    if (static_cast<u32>(permissions) & ~static_cast<u32>(own_other_permissions)) {
        LOG_ERROR(Kernel, "cannot map id={}, address=0x{:08X} name={}, permissions don't match",
                  GetObjectId(), address, name);
        return ERR_INVALID_COMBINATION;
    }

    // Heap-backed memory blocks can not be mapped with other_permissions = DontCare
    if (base_address != 0 && other_permissions == MemoryPermission::DontCare) {
        LOG_ERROR(Kernel, "cannot map id={}, address=0x{08X} name={}, permissions don't match",
                  GetObjectId(), address, name);
        return ERR_INVALID_COMBINATION;
    }

    // Error out if the provided permissions are not compatible with what the creator process needs.
    if (other_permissions != MemoryPermission::DontCare &&
        static_cast<u32>(this->permissions) & ~static_cast<u32>(other_permissions)) {
        LOG_ERROR(Kernel, "cannot map id={}, address=0x{:08X} name={}, permissions don't match",
                  GetObjectId(), address, name);
        return ERR_WRONG_PERMISSION;
    }

    // TODO(Subv): Check for the Shared Device Mem flag in the creator process.
    /*if (was_created_with_shared_device_mem && address != 0) {
        return ResultCode(ErrorDescription::InvalidCombination, ErrorModule::OS,
    ErrorSummary::InvalidArgument, ErrorLevel::Usage);
    }*/

    // TODO(Subv): The same process that created a SharedMemory object
    // can not map it in its own address space unless it was created with addr=0, result 0xD900182C.

    if (address != 0) {
        if (address < Memory::HEAP_VADDR || address + size >= Memory::SHARED_MEMORY_VADDR_END) {
            LOG_ERROR(Kernel, "cannot map id={}, address=0x{:08X} name={}, invalid address",
                      GetObjectId(), address, name);
            return ERR_INVALID_ADDRESS;
        }
    }

    VAddr target_address = address;

    if (base_address == 0 && target_address == 0) {
        // Calculate the address at which to map the memory block.
        // Note: even on new firmware versions, the target address is still in the old linear heap
        // region. This exception is made to keep the shared font compatibility. See
        // APT:GetSharedFont for detail.
        target_address = linear_heap_phys_offset + Memory::LINEAR_HEAP_VADDR;
    }

    auto vma = target_process.vm_manager.FindVMA(target_address);
    if (vma->second.type != VMAType::Free ||
        vma->second.base + vma->second.size < target_address + size) {
        LOG_ERROR(Kernel,
                  "cannot map id={}, address=0x{:08X} name={}, mapping to already allocated memory",
                  GetObjectId(), address, name);
        return ERR_INVALID_ADDRESS_STATE;
    }

    // Map the memory block into the target process
    VAddr interval_target = target_address;
    for (const auto& interval : backing_blocks) {
        auto vma = target_process.vm_manager.MapBackingMemory(interval_target, interval.first,
                                                              interval.second, MemoryState::Shared);
        ASSERT(vma.Succeeded());
        target_process.vm_manager.Reprotect(vma.Unwrap(), ConvertPermissions(permissions));
        interval_target += interval.second;
    }

    return RESULT_SUCCESS;
}

ResultCode SharedMemory::Unmap(Process& target_process, VAddr address) {
    // TODO(Subv): Verify what happens if the application tries to unmap an address that is not
    // mapped to a SharedMemory.
    return target_process.vm_manager.UnmapRange(address, size);
}

VMAPermission SharedMemory::ConvertPermissions(MemoryPermission permission) {
    u32 masked_permissions =
        static_cast<u32>(permission) & static_cast<u32>(MemoryPermission::ReadWriteExecute);
    return static_cast<VMAPermission>(masked_permissions);
};

u8* SharedMemory::GetPointer(u32 offset) {
    if (backing_blocks.size() != 1) {
        LOG_WARNING(Kernel, "Unsafe GetPointer on discontinuous SharedMemory");
    }
    return backing_blocks[0].first + offset;
}

const u8* SharedMemory::GetPointer(u32 offset) const {
    if (backing_blocks.size() != 1) {
        LOG_WARNING(Kernel, "Unsafe GetPointer on discontinuous SharedMemory");
    }
    return backing_blocks[0].first + offset;
}

} // namespace Kernel
