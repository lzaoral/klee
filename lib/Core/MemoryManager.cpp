//===-- MemoryManager.cpp -------------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "CoreStats.h"
#include "Memory.h"
#include "MemoryManager.h"

#include "klee/Expr.h"
#include "klee/Internal/Support/ErrorHandling.h"

#include "llvm/Support/CommandLine.h"
#include "llvm/Support/MathExtras.h"

#include <inttypes.h>
#include <sys/mman.h>

using namespace klee;

namespace {
llvm::cl::opt<bool> DeterministicAllocation(
    "allocate-determ",
    llvm::cl::desc("Allocate memory deterministically(default=off)"),
    llvm::cl::init(false));

llvm::cl::opt<unsigned> DeterministicAllocationSize(
    "allocate-determ-size",
    llvm::cl::desc(
        "Preallocated memory for deterministic allocation in MB (default=100)"),
    llvm::cl::init(100));

llvm::cl::opt<bool>
    NullOnZeroMalloc("return-null-on-zero-malloc",
                     llvm::cl::desc("Returns NULL in case malloc(size) was "
                                    "called with size 0 (default=off)."),
                     llvm::cl::init(false));

llvm::cl::opt<unsigned> RedZoneSpace(
    "red-zone-space",
    llvm::cl::desc("Set the amount of free space between allocations. This is "
                   "important to detect out-of-bound accesses (default=10)."),
    llvm::cl::init(10));

llvm::cl::opt<unsigned long long> DeterministicStartAddress(
    "allocate-determ-start-address",
    llvm::cl::desc("Start address for deterministic allocation. Has to be page "
                   "aligned (default=0x7ff30000000)."),
    llvm::cl::init(0x7ff30000000));
}

/***/
MemoryManager::MemoryManager(ArrayCache *_arrayCache,
                             unsigned ptrWidth)
    : arrayCache(_arrayCache), deterministicSpace(0), nextFreeSlot(0),
      spaceSize(DeterministicAllocationSize.getValue() * 1024 * 1024),
      pointerBitWidth(ptrWidth), lastSegment(0) {
  if (DeterministicAllocation) {
    // Page boundary
    void *expectedAddress = (void *)DeterministicStartAddress.getValue();

    unsigned flags = MAP_ANONYMOUS | MAP_PRIVATE;
    if (ptrWidth == 32)
        flags |= MAP_32BIT;

    char *newSpace =
        (char *)mmap(expectedAddress, spaceSize,

                     PROT_READ | PROT_WRITE,
                     flags, -1, 0);

    if (newSpace == MAP_FAILED) {
      klee_error("Couldn't mmap() memory for deterministic allocations");
    }
    if (expectedAddress != newSpace && expectedAddress != 0) {
      klee_error("Could not allocate memory deterministically");
    }

    klee_message("Deterministic memory allocation starting from %p", newSpace);
    deterministicSpace = newSpace;
    nextFreeSlot = newSpace;
  }
}

static void free_address(MemoryObject *mo, bool low_address)
{
  size_t alloc_size = 1;
  if (ConstantExpr *CE = dyn_cast<ConstantExpr>(mo->size))
    alloc_size = CE->getZExtValue();
  if (low_address)
    munmap((void *)mo->address, alloc_size);
  else
    free((void *) mo->address);
}

MemoryManager::~MemoryManager() {
  while (!objects.empty()) {
    MemoryObject *mo = *objects.begin();
    if (!mo->isFixed && !DeterministicAllocation)
      free_address(mo, pointerBitWidth == 32);
    objects.erase(mo);
    delete mo;
  }

  if (DeterministicAllocation)
    munmap(deterministicSpace, spaceSize);
}

static uint64_t allocate_memory(uint64_t size, bool low_address = false)
{
  void *mem;
  if (low_address) {
    mem = mmap(0, size, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    if (mem == MAP_FAILED)
      return 0;
  } else
    mem = malloc(size);

  return (uint64_t) mem;
}

MemoryObject *MemoryManager::allocate(uint64_t size, bool isLocal,
                                      bool isGlobal,
                                      const llvm::Value *allocSite,
                                      size_t alignment) {
  ref<Expr> sizeExpr = ConstantExpr::alloc(size, Context::get().getPointerWidth());
  return allocate(sizeExpr, isLocal, isGlobal, allocSite, alignment);
}

MemoryObject *MemoryManager::allocate(ref<Expr> size, bool isLocal,
                                      bool isGlobal,
                                      const llvm::Value *allocSite,
                                      size_t alignment) {
  uint64_t concreteSize = 0;
  bool hasConcreteSize = false;
  if (ConstantExpr *CE = dyn_cast<ConstantExpr>(size)) {
    hasConcreteSize = true;
    concreteSize = CE->getZExtValue();
  }

  if (concreteSize > 10 * 1024 * 1024)
    klee_warning_once(0, "Large alloc: %" PRIu64
                         " bytes.  KLEE may run out of memory.",
                      concreteSize);

  // Return NULL if size is zero, this is equal to error during allocation
  if (NullOnZeroMalloc && hasConcreteSize && concreteSize == 0)
    return 0;

  if (!llvm::isPowerOf2_64(alignment)) {
    klee_warning("Only alignment of power of two is supported");
    return 0;
  }

  uint64_t address = 0;
  if (DeterministicAllocation) {
#if LLVM_VERSION_CODE >= LLVM_VERSION(3, 9)
    address = llvm::alignTo((uint64_t)nextFreeSlot + alignment - 1, alignment);
#else
    address = llvm::RoundUpToAlignment((uint64_t)nextFreeSlot + alignment - 1,
                                       alignment);
#endif

    // Handle the case of 0-sized allocations as 1-byte allocations.
    // This way, we make sure we have this allocation between its own red zones
    size_t alloc_size = std::max(concreteSize, (uint64_t)1);
    if ((char *)address + alloc_size < deterministicSpace + spaceSize) {
      nextFreeSlot = (char *)address + alloc_size + RedZoneSpace;
    } else {
      klee_warning_once(0, "Couldn't allocate %" PRIu64
                           " bytes. Not enough deterministic space left.",
                        concreteSize);
      address = 0;
    }
  } else {
    // allocate 1 byte for symbolic-size allocation, just so we get an address
    size_t alloc_size = hasConcreteSize ? concreteSize : 1;
    // Use malloc for the standard case
    if (alignment <= 8 || pointerBitWidth == 32) {
      address = allocate_memory(alloc_size, pointerBitWidth == 32);
      if (address == 0 && concreteSize != 0) {
          klee_warning("Allocating memory failed.");
          return 0;
      }
      assert(pointerBitWidth > 32 || address < (1L << 32));
    } else {
      int res = posix_memalign((void **)&address, alignment, alloc_size);
      if (res < 0) {
        klee_warning("Allocating aligned memory failed.");
        address = 0;
      }
    }
  }

  if (!address)
    return 0;

  ++stats::allocations;
  MemoryObject *res = new MemoryObject(++lastSegment, address, size, isLocal,
                                       isGlobal, false, allocSite, this);
  objects.insert(res);
  return res;
}

MemoryObject *MemoryManager::allocateFixed(uint64_t address, uint64_t size,
                                           const llvm::Value *allocSite) {
#ifndef NDEBUG
  for (objects_ty::iterator it = objects.begin(), ie = objects.end(); it != ie;
       ++it) {
    MemoryObject *mo = *it;
    // symbolic size objects can overlap
    if (ConstantExpr *CE = dyn_cast<ConstantExpr>(mo->size)) {
      unsigned moSize = CE->getZExtValue();
      if (address + moSize > mo->address && address < mo->address + moSize)
        klee_error("Trying to allocate an overlapping object");
    }
  }
#endif

  ++stats::allocations;
  ref<Expr> sizeExpr = ConstantExpr::alloc(size, Context::get().getPointerWidth());
  MemoryObject *res =
      new MemoryObject(++lastSegment, address, sizeExpr, false, true, true, allocSite, this);
  objects.insert(res);
  return res;
}

void MemoryManager::deallocate(const MemoryObject *mo) { assert(0); }

void MemoryManager::markFreed(MemoryObject *mo) {
  if (objects.find(mo) != objects.end()) {
    if (!mo->isFixed && !DeterministicAllocation)
      free_address(mo, pointerBitWidth == 32);

    objects.erase(mo);
  }
}

size_t MemoryManager::getUsedDeterministicSize() {
  return nextFreeSlot - deterministicSpace;
}
