//===-- llvm/Target/TargetFrameInfo.h ---------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Interface to describe the layout of a stack frame on the target machine.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TARGET_TARGETFRAMEINFO_H
#define LLVM_TARGET_TARGETFRAMEINFO_H

#include <utility>
#include <vector>

namespace llvm {
  class MachineFunction;
  class MachineBasicBlock;
  class MachineMove;

/// Information about stack frame layout on the target.  It holds the direction
/// of stack growth, the known stack alignment on entry to each function, and
/// the offset to the locals area.
///
/// The offset to the local area is the offset from the stack pointer on
/// function entry to the first location where function data (local variables,
/// spill locations) can be stored.
class TargetFrameInfo {
public:
  enum StackDirection {
    StackGrowsUp,        // Adding to the stack increases the stack address
    StackGrowsDown       // Adding to the stack decreases the stack address
  };

  // Maps a callee saved register to a stack slot with a fixed offset.
  struct SpillSlot {
    unsigned Reg;
    int Offset; // Offset relative to stack pointer on function entry.
  };
private:
  StackDirection StackDir;
  unsigned StackAlignment;
  unsigned TransientStackAlignment;
  int LocalAreaOffset;
public:
  TargetFrameInfo(StackDirection D, unsigned StackAl, int LAO,
                  unsigned TransAl = 1)
    : StackDir(D), StackAlignment(StackAl), TransientStackAlignment(TransAl),
      LocalAreaOffset(LAO) {}

  virtual ~TargetFrameInfo();

  // These methods return information that describes the abstract stack layout
  // of the target machine.

  /// getStackGrowthDirection - Return the direction the stack grows
  ///
  StackDirection getStackGrowthDirection() const { return StackDir; }

  /// getStackAlignment - This method returns the number of bytes to which the
  /// stack pointer must be aligned on entry to a function.  Typically, this
  /// is the largest alignment for any data object in the target.
  ///
  unsigned getStackAlignment() const { return StackAlignment; }

  /// getTransientStackAlignment - This method returns the number of bytes to
  /// which the stack pointer must be aligned at all times, even between
  /// calls.
  ///
  unsigned getTransientStackAlignment() const {
    return TransientStackAlignment;
  }

  /// getOffsetOfLocalArea - This method returns the offset of the local area
  /// from the stack pointer on entrance to a function.
  ///
  int getOffsetOfLocalArea() const { return LocalAreaOffset; }

  /// getCalleeSavedSpillSlots - This method returns a pointer to an array of
  /// pairs, that contains an entry for each callee saved register that must be
  /// spilled to a particular stack location if it is spilled.
  ///
  /// Each entry in this array contains a <register,offset> pair, indicating the
  /// fixed offset from the incoming stack pointer that each register should be
  /// spilled at. If a register is not listed here, the code generator is
  /// allowed to spill it anywhere it chooses.
  ///
  virtual const SpillSlot *
  getCalleeSavedSpillSlots(unsigned &NumEntries) const {
    NumEntries = 0;
    return 0;
  }

  /// targetHandlesStackFrameRounding - Returns true if the target is
  /// responsible for rounding up the stack frame (probably at emitPrologue
  /// time).
  virtual bool targetHandlesStackFrameRounding() const {
    return false;
  }

  /// emitProlog/emitEpilog - These methods insert prolog and epilog code into
  /// the function.
  virtual void emitPrologue(MachineFunction &MF) const = 0;
  virtual void emitEpilogue(MachineFunction &MF,
                            MachineBasicBlock &MBB) const = 0;

  /// hasFP - Return true if the specified function should have a dedicated
  /// frame pointer register. For most targets this is true only if the function
  /// has variable sized allocas or if frame pointer elimination is disabled.
  virtual bool hasFP(const MachineFunction &MF) const = 0;

  /// hasReservedCallFrame - Under normal circumstances, when a frame pointer is
  /// not required, we reserve argument space for call sites in the function
  /// immediately on entry to the current function. This eliminates the need for
  /// add/sub sp brackets around call sites. Returns true if the call frame is
  /// included as part of the stack frame.
  virtual bool hasReservedCallFrame(const MachineFunction &MF) const {
    return !hasFP(MF);
  }

  /// canSimplifyCallFramePseudos - When possible, it's best to simplify the
  /// call frame pseudo ops before doing frame index elimination. This is
  /// possible only when frame index references between the pseudos won't
  /// need adjusting for the call frame adjustments. Normally, that's true
  /// if the function has a reserved call frame or a frame pointer. Some
  /// targets (Thumb2, for example) may have more complicated criteria,
  /// however, and can override this behavior.
  virtual bool canSimplifyCallFramePseudos(const MachineFunction &MF) const {
    return hasReservedCallFrame(MF) || hasFP(MF);
  }

  /// getInitialFrameState - Returns a list of machine moves that are assumed
  /// on entry to all functions.  Note that LabelID is ignored (assumed to be
  /// the beginning of the function.)
  virtual void getInitialFrameState(std::vector<MachineMove> &Moves) const;
};

} // End llvm namespace

#endif
