/* Copyright 2020 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow/compiler/xla/service/memory_space_assignment_utils.h"

#include "tensorflow/compiler/xla/service/hlo_casting_utils.h"
#include "tensorflow/compiler/xla/service/hlo_instructions.h"

namespace xla {

bool MemorySpaceAssignmentUtils::IsValueAllowedInAlternateMemory(
    const HloValue* value) {
  // If the buffer is a tuple, don't use this algorithm for now. The buffers
  // that are pointed to by the tuple will still use this algorithm.  Because
  // tuples are cheap to place in the alternate memory (they are just pointers)
  // we don't need to use prefetch/evict logic.
  if (value->shape().IsTuple()) {
    VLOG(4) << "Keeping value " << value->ToShortString()
            << " in default mem because it is a tuple.";
    return false;
  }

  // Don't place scalars in the alternate memory.
  if (ShapeUtil::IsEffectiveScalar(value->shape())) {
    VLOG(4) << "Keeping value " << value->ToShortString()
            << " in default mem because it is a scalar.";
    return false;
  }

  // TODO(berkin): Not allocating add-dependencies either since they need to be
  // treated specially. We should revisit this later.
  for (const HloPosition& position : value->positions()) {
    if (position.instruction->opcode() == HloOpcode::kAddDependency) {
      VLOG(4) << "Keeping value " << value->ToShortString()
              << " in default mem because it has a "
              << "add-dependency position.";
      return false;
    }
  }

  // Send and Recv HLOs return a request identifier. These should not be
  // allocated in the alternate memory.
  for (const HloPosition& position : value->positions()) {
    if ((position.instruction->opcode() == HloOpcode::kSend ||
         position.instruction->opcode() == HloOpcode::kRecv) &&
        DynCast<HloSendRecvInstruction>(position.instruction)
            ->is_host_transfer()) {
      // TODO(berkin): Host transfers using alternate memory space doesn't seem
      // to work at the moment.
      VLOG(4) << "Keeping value " << value->ToShortString()
              << " in default mem because it is a send/recv buffer used for "
                 "host transfer.";
      return false;
    }

    if (auto* custom_call =
            DynCast<HloCustomCallInstruction>(position.instruction)) {
      for (const auto& pair : custom_call->output_to_operand_aliasing()) {
        if (position.index == pair.first) {
          VLOG(4) << "Keeping value " << value->ToShortString()
                  << " in default mem because it is a custom-call output that "
                     "aliases an operand buffer.";
          return false;
        }
      }
    }
  }

  return true;
}

bool MemorySpaceAssignmentUtils::IsIntervalAllowedInAlternateMemory(
    const GlobalDecreasingSizeBestFitHeap<HloValue>::BufferInterval& interval) {
  return IsValueAllowedInAlternateMemory(interval.buffer) &&
         absl::c_all_of(interval.colocations, IsValueAllowedInAlternateMemory);
}

/*static*/ void MemorySpaceAssignmentUtils::HoistConstantOperations(
    HloModule& module) {
  CHECK(module.has_schedule());
  HloSchedule& schedule = module.schedule();
  for (const HloComputation* computation : module.MakeNonfusionComputations()) {
    CHECK(schedule.is_computation_scheduled(computation));
    const HloInstructionSequence& sequence = schedule.sequence(computation);
    HloInstructionSequence new_sequence;

    for (HloInstruction* instruction : sequence.instructions()) {
      if (instruction->opcode() == HloOpcode::kConstant) {
        new_sequence.push_back(instruction);
      }
    }
    for (HloInstruction* instruction : sequence.instructions()) {
      if (instruction->opcode() != HloOpcode::kConstant) {
        new_sequence.push_back(instruction);
      }
    }
    CHECK_EQ(new_sequence.size(), sequence.size());
    schedule.set_sequence(computation, new_sequence);
  }
}

}  // namespace xla
