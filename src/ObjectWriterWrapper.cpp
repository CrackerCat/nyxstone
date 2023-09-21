#include "ObjectWriterWrapper.h"

#include "nyxstone.h"
#include <algorithm>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "Target/AArch64/MCTargetDesc/AArch64FixupKinds.h"
#include "Target/AArch64/MCTargetDesc/AArch64MCExpr.h"
#include "Target/ARM/MCTargetDesc/ARMFixupKinds.h"
#pragma GCC diagnostic pop

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include <llvm/MC/MCAsmBackend.h>
#include <llvm/MC/MCAsmLayout.h>
#include <llvm/MC/MCAssembler.h>
#include <llvm/MC/MCContext.h>
#include <llvm/MC/MCExpr.h>
#include <llvm/MC/MCFixup.h>
#include <llvm/MC/MCFixupKindInfo.h>
#include <llvm/MC/MCFragment.h>
#include <llvm/MC/MCObjectWriter.h>
#include <llvm/MC/MCSection.h>
#include <llvm/MC/MCSymbol.h>
#include <llvm/MC/MCValue.h>
#include <llvm/Support/Casting.h>
#pragma GCC diagnostic pop

#include <sstream>

using namespace llvm;

/// Performs a value alignment to a specific value.
///
/// @param[in] value - Value to align.
/// @param[in] alignment - Alignment to align the value.
/// @returns Aligned value.
uint64_t alignUp(uint64_t value, uint64_t alignment) {
    auto remainder = value % alignment;
    return (remainder > 0) ? value + (alignment - remainder) : value;
}

void validate_arm_thumb(const MCFixup& fixup, const MCAsmLayout& Layout, MCContext& context) {
    // For all instructions we are checking here, we need to make sure that the fixup is a SymbolRef.
    // If it is not, we do not need to check the instruction.
    const bool fixup_is_symbolref = fixup.getValue() != nullptr && fixup.getValue()->getKind() == MCExpr::SymbolRef;
    if (!fixup_is_symbolref) {
        return;
    }

    // Check for missaligned target address in 2-byte `ADR` and `LDR` instructions that only allow multiples of four.
    if (fixup.getTargetKind() == ARM::fixup_thumb_adr_pcrel_10 || fixup.getTargetKind() == ARM::fixup_arm_thumb_cp) {
        const auto& symbol = cast<const MCSymbolRefExpr>(fixup.getValue())->getSymbol();
        auto address = Layout.getFragmentOffset(symbol.getFragment()) + symbol.getOffset();

        // Check that target is 4-byte aligned
        if ((address & 3U) != 0) {
            context.reportError(fixup.getLoc(), "misaligned label address (reported by nyxstone)");
        }
    }

    // Check for out-of-bounds ARM Thumb2 `ADR` instruction
    if (fixup.getTargetKind() == ARM::fixup_t2_adr_pcrel_12) {
        auto offset = static_cast<int64_t>(cast<const MCSymbolRefExpr>(fixup.getValue())->getSymbol().getOffset());
        offset -= 4;  // Source address is (PC + 4)

        // Check min/max bounds of instruction encoding
        // Symmetric bounds as `addw` and `subw` are used internally
        if (offset <= -4096 || offset >= 4096) {
            context.reportError(fixup.getLoc(), "out of range pc-relative fixup value (reported by Nyxstone)");
        }
    }

    // Check for misaligned target address for all ARM Thumb branch instructions
    if (fixup.getTargetKind() == ARM::fixup_arm_thumb_br || fixup.getTargetKind() == ARM::fixup_arm_thumb_bl
        || fixup.getTargetKind() == ARM::fixup_arm_thumb_bcc || fixup.getTargetKind() == ARM::fixup_t2_uncondbranch
        || fixup.getTargetKind() == ARM::fixup_t2_condbranch) {
        auto& symbol = cast<const MCSymbolRefExpr>(fixup.getValue())->getSymbol();
        auto address = Layout.getFragmentOffset(symbol.getFragment()) + symbol.getOffset();

        // Check that target is 2-byte aligned
        if ((address & 1) != 0) {
            context.reportError(fixup.getLoc(), "misaligned label address (reported by nyxstone)");
        }
    }

    // Check for out-of-bounds and misaligned label for ARM Thumb2 'LDC' instruction
    if (fixup.getTargetKind() == ARM::fixup_t2_pcrel_10) {
        auto& symbol = cast<const MCSymbolRefExpr>(fixup.getValue())->getSymbol();
        auto address = Layout.getFragmentOffset(symbol.getFragment()) + symbol.getOffset();

        auto offset = static_cast<int64_t>(symbol.getOffset()) - 4;  // Source address is (PC + 4)

        // Since llvm only wrongly assembles for offsets which differ from the allowed value for delta < 4
        // it is enough to check that the offset is validly aligned to 4. For better error reporting,
        // we still check the offsets here.
        if (offset < -1020 || offset > 1020) {
            context.reportError(fixup.getLoc(), "out of range pc-relative fixup value (reported by Nyxstone)");
        }

        // check that target is 4 byte aligned
        if ((address & 3) != 0) {
            context.reportError(fixup.getLoc(), "misaligned label address (reported by Nyxstone)");
        }
    }
}

void validate_aarch64(const MCFixup& fixup, [[maybe_unused]] const MCAsmLayout& Layout, MCContext& context) {
    // Check for out-of-bounds AArch64 `ADR` instruction
    if (context.getTargetTriple().isAArch64() && fixup.getTargetKind() == AArch64::fixup_aarch64_pcrel_adr_imm21
        && fixup.getValue() != nullptr && fixup.getValue()->getKind() == MCExpr::Target
        && cast<const AArch64MCExpr>(fixup.getValue())->getSubExpr() != nullptr
        && cast<const AArch64MCExpr>(fixup.getValue())->getSubExpr()->getKind() == MCExpr::SymbolRef) {
        const auto* const sub_expr = cast<const AArch64MCExpr>(fixup.getValue())->getSubExpr();
        const auto offset = static_cast<int64_t>(cast<const MCSymbolRefExpr>(sub_expr)->getSymbol().getOffset());

        // Check min/max bounds of instruction encoding
        // Asymmetric bounds as two's complement is used
        if (offset < -0x100000 || offset >= 0x100000) {
            context.reportError(fixup.getLoc(), "fixup value out of range (reported by Nyxstone)");
        }
    }
}

/// Additional validation checks for fixups.
/// For some fixup kinds LLVM is missing out-of-bounds and alignment checks and
/// produces wrong instruction bytes instead of an error message.
void ObjectWriterWrapper::validate_fixups(const MCFragment& fragment, const MCAsmLayout& Layout) {
    // Get fixups
    const SmallVectorImpl<MCFixup>* fixups = nullptr;
    switch (fragment.getKind()) {
        default:
            return;
        case MCFragment::FT_Data: {
            fixups = &cast<const MCDataFragment>(fragment).getFixups();
            break;
        }
        case MCFragment::FT_Relaxable: {
            fixups = &cast<const MCRelaxableFragment>(fragment).getFixups();
            break;
        }
    }

    // Iterate fixups
    for (const auto& fixup : *fixups) {
        // Additional validations for ARM Thumb instructions
        if (is_ArmT16_or_ArmT32(context.getTargetTriple())) {
            validate_arm_thumb(fixup, Layout, context);
        }

        // Additional validations for AArch64 instructions
        if (context.getTargetTriple().isAArch64()) {
            validate_aarch64(fixup, Layout, context);
        }
    }
}

void ObjectWriterWrapper::executePostLayoutBinding(llvm::MCAssembler& Asm, const llvm::MCAsmLayout& Layout) {
    inner_object_writer->executePostLayoutBinding(Asm, Layout);
}

void ObjectWriterWrapper::recordRelocation(
    MCAssembler& Asm,
    const MCAsmLayout& Layout,
    const MCFragment* Fragment,
    const MCFixup& Fixup,
    MCValue Target,
    uint64_t& FixedValue) {
    inner_object_writer->recordRelocation(Asm, Layout, Fragment, Fixup, Target, FixedValue);

    // LLVM performs relocation for the AArch64 instruction `adrp` during the linking step.
    // Therefore, we need to perform the relocation ourselves.
    // Semantics: REG := PC + .LABEL on 4k aligned page
    if (context.getTargetTriple().isAArch64()) {
        auto const IsPCRel = Asm.getBackend().getFixupKindInfo(Fixup.getKind()).Flags & MCFixupKindInfo::FKF_IsPCRel;
        auto const kind = Fixup.getTargetKind();

        if ((IsPCRel != 0U) && kind == AArch64::fixup_aarch64_pcrel_adrp_imm21) {
            const auto* const aarch64_expr = cast<AArch64MCExpr>(Fixup.getValue());
            const auto* const symbol_ref = cast<MCSymbolRefExpr>(aarch64_expr->getSubExpr());
            auto const& symbol = symbol_ref->getSymbol();

            // Perform the fixup
            const int64_t PAGE_SIZE = 0x1000;
            FixedValue = alignUp(symbol.getOffset(), PAGE_SIZE);
        }
    }
}

uint64_t ObjectWriterWrapper::writeObject(MCAssembler& Asm, const MCAsmLayout& Layout) {
    // Get .text section
    const auto& sections = Layout.getSectionOrder();
    const MCSection* const* text_section_it = std::find_if(std::begin(sections), std::end(sections), [](auto section) {
        return section->getName().str() == ".text";
    });

    if (text_section_it == sections.end()) {
        extended_error += "[writeObject] Object has no .text section.";
        return 0;
    }

    const MCSection* text_section = *text_section_it;

    // Iterate fragments
    size_t curr_insn = 0;
    for (const MCFragment& fragment : *text_section) {
        // Additional validation of fixups that LLVM is missing
        validate_fixups(fragment, Layout);

        // If requested, do post processing of instruction details (that corrects for relocations and applied fixups)
        if (instructions == nullptr) {
            continue;
        }

        // Data fragment may contain multiple instructions that did not change in size
        if (fragment.getKind() == MCFragment::FT_Data) {
            const auto& data_fragment = cast<MCDataFragment>(fragment);
            const ArrayRef<char> contents = data_fragment.getContents();

            // Update bytes of multiple instructions
            size_t frag_pos = 0;
            while (true) {
                auto& insn_bytes = instructions->at(curr_insn).bytes;
                auto insn_len = insn_bytes.size();

                // Check if fragment contains this instruction
                if (frag_pos + insn_len > contents.size()) {
                    break;
                }

                // Update instruction bytes
                insn_bytes.clear();
                insn_bytes.reserve(insn_len);
                std::copy(
                    contents.begin() + frag_pos,
                    contents.begin() + frag_pos + insn_len,
                    std::back_inserter(insn_bytes));

                // Prepare next iteration
                frag_pos += insn_len;
                curr_insn++;

                // Check if more instructions to be updated exist
                if (curr_insn >= instructions->size()) {
                    break;
                }
            }
        } else if (fragment.getKind() == MCFragment::FT_Relaxable) {
            // Relaxable fragment contains exactly one instruction that may have increased in size
            const auto& relaxable_fragment = cast<MCRelaxableFragment>(fragment);
            const ArrayRef<char> contents = relaxable_fragment.getContents();

            // Update instruction bytes
            auto& insn_bytes = instructions->at(curr_insn).bytes;
            insn_bytes.clear();
            insn_bytes.reserve(contents.size());
            std::copy(contents.begin(), contents.end(), std::back_inserter(insn_bytes));

            // Prepare next iteration
            curr_insn++;
        }

        // Check if more instructions to be updated exist
        if (curr_insn >= instructions->size()) {
            break;
        }
    }

    // Generate output
    if (write_text_section_only) {
        // Write .text section bytes only
        const uint64_t start = stream.tell();
        Asm.writeSectionData(stream, text_section, Layout);
        return stream.tell() - start;
    }

    // Write complete object file
    return inner_object_writer->writeObject(Asm, Layout);
}

std::unique_ptr<MCObjectWriter> createObjectWriterWrapper(
    std::unique_ptr<MCObjectWriter>&& object_writer,
    raw_pwrite_stream& stream,
    MCContext& context,
    bool write_text_section_only,
    std::string& extended_error,
    std::vector<Nyxstone::Instruction>* instructions) {
    return std::make_unique<ObjectWriterWrapper>(
        std::move(object_writer),
        stream,
        context,
        write_text_section_only,
        extended_error,
        instructions);
}