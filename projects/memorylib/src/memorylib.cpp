#include "memorylib/memorylib.hpp"

#include <Zydis/Zydis.h>

#include <MinHook.h>
#include <winnt.h>

#include <algorithm>
#include <charconv>
#include <cstring>
#include <limits>
#include <utility>

namespace mem {

bool AddressRange::contains(const void *address, std::size_t size) const noexcept {
    if (!valid() || !address || size > static_cast<std::size_t>(end - begin))
        return false;
    const auto p = static_cast<const std::uint8_t *>(address);
    return p >= begin && p <= end && size <= static_cast<std::size_t>(end - p);
}

bool ProcessMemory::readable(const void *address, std::size_t size) noexcept {
    if (!address || size == 0)
        return false;
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(address, &info, sizeof(info)))
        return false;
    if (info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
        return false;
    const auto start = reinterpret_cast<std::uintptr_t>(address);
    const auto finish = start + size;
    const auto region_end = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
    return finish >= start && finish <= region_end;
}

HMODULE module_handle(std::wstring_view name) {
    if (name.empty())
        return GetModuleHandleW(nullptr);
    const std::wstring owned_name(name);
    return GetModuleHandleW(owned_name.c_str());
}

void *export_address(HMODULE module, std::string_view name) {
    if (!module || name.empty())
        return nullptr;
    const std::string owned_name(name);
    return reinterpret_cast<void *>(GetProcAddress(module, owned_name.c_str()));
}

Result<std::vector<const std::uint8_t *>> walk_circular_list(const void *owner, CircularListLayout layout, std::size_t max_nodes) {
    if (!owner || !max_nodes)
        return failure<std::vector<const std::uint8_t *>>(Diagnostic::Code::invalid_argument, "circular list", "invalid owner or node limit");
    const auto owner_bytes = static_cast<const std::uint8_t *>(owner);
    auto sentinel_result = read<const std::uint8_t *>(owner_bytes + layout.sentinel_offset);
    if (!sentinel_result || !sentinel_result.get())
        return failure<std::vector<const std::uint8_t *>>(Diagnostic::Code::protection, "circular list", "sentinel pointer is not readable");

    const auto sentinel = sentinel_result.get();
    const auto required = std::max(layout.node_size, layout.next_offset + sizeof(void *));
    if (!ProcessMemory::readable(sentinel, required))
        return failure<std::vector<const std::uint8_t *>>(Diagnostic::Code::protection, "circular list", "sentinel node is not readable");
    auto node_result = read<const std::uint8_t *>(sentinel + layout.next_offset);
    if (!node_result || !node_result.get())
        return failure<std::vector<const std::uint8_t *>>(Diagnostic::Code::protection, "circular list", "first node pointer is not readable");

    std::vector<const std::uint8_t *> nodes;
    auto node = node_result.get();
    for (std::size_t count = 0; count < max_nodes && node != sentinel; ++count) {
        if (!node || !ProcessMemory::readable(node, required))
            return failure<std::vector<const std::uint8_t *>>(Diagnostic::Code::protection, "circular list", "node is not readable", reinterpret_cast<std::uintptr_t>(node));
        nodes.push_back(node);
        auto next_result = read<const std::uint8_t *>(node + layout.next_offset);
        if (!next_result || !next_result.get())
            return failure<std::vector<const std::uint8_t *>>(Diagnostic::Code::protection, "circular list", "next node pointer is not readable",
                reinterpret_cast<std::uintptr_t>(node));
        node = next_result.get();
    }
    if (node != sentinel)
        return failure<std::vector<const std::uint8_t *>>(Diagnostic::Code::ambiguous, "circular list", "list did not return to its sentinel");
    return success(std::move(nodes));
}

Result<ModuleSections> ModuleView::sections(DiagnosticSink sink) const {
    if (!module_)
        return failure<ModuleSections>(Diagnostic::Code::invalid_module, "module", "module handle is null");
    auto base = reinterpret_cast<std::uint8_t *>(module_);
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
    if (!ProcessMemory::readable(dos, sizeof(*dos)) || dos->e_magic != IMAGE_DOS_SIGNATURE)
        return failure<ModuleSections>(Diagnostic::Code::invalid_module, "module", "invalid DOS header");
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
    if (!ProcessMemory::readable(nt, sizeof(*nt)) || nt->Signature != IMAGE_NT_SIGNATURE)
        return failure<ModuleSections>(Diagnostic::Code::invalid_module, "module", "invalid NT header");

    ModuleSections result{};
    result.module = module_;
    result.image = {base, base + nt->OptionalHeader.SizeOfImage};
    auto section = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        AddressRange range{base + section->VirtualAddress, base + section->VirtualAddress + std::max(section->Misc.VirtualSize, section->SizeOfRawData)};
        char name[9]{};
        std::memcpy(name, section->Name, 8);
        if (std::strcmp(name, ".text") == 0)
            result.text = range;
        if (std::strcmp(name, ".rdata") == 0)
            result.rdata = range;
    }
    if (!result.text.valid() || !result.rdata.valid())
        return failure<ModuleSections>(Diagnostic::Code::not_found, "module", "required PE sections were not found");
    if (sink)
        sink({"module", "PE sections resolved", reinterpret_cast<std::uintptr_t>(module_)});
    return success(result);
}

Result<Scan> Scan::open(std::wstring_view module_name, DiagnosticSink sink) {
    const auto module = module_handle(module_name);
    if (!module)
        return failure<Scan>(Diagnostic::Code::invalid_module, "module", "module is not loaded");

    auto sections = ModuleView(module).sections(sink);
    if (!sections)
        return sections.forward_error<Scan>("module sections");
    return success(Scan(sections.get(), std::move(sink)));
}

Result<AddressRange> Resolver::function_range(const std::uint8_t *address, std::string_view stage) const {
    auto bounds = mem::function_bounds(address, sections_.module);
    if (!bounds)
        return bounds.forward_error<AddressRange>(std::string(stage));
    return success(AddressRange{bounds->first, bounds->first + bounds->second});
}

Result<AddressRange> Scan::function_range(const std::uint8_t *address, std::string_view stage) const {
    return resolver_.function_range(address, stage);
}

Result<std::vector<const std::uint8_t *>> Scan::find(const AddressRange &range, std::string_view ida_pattern) const {
    auto parsed = Pattern::parse(ida_pattern);
    if (!parsed)
        return parsed.forward_error<std::vector<const std::uint8_t *>>("pattern scan");
    return success(find_pattern(range, parsed.get()));
}

Result<Pattern> Pattern::parse(std::string_view input) {
    Pattern pattern{};
    std::size_t pos = 0;
    while (pos < input.size()) {
        while (pos < input.size() && (input[pos] == ' ' || input[pos] == '\t'))
            ++pos;
        if (pos == input.size())
            break;
        if (input[pos] == '?') {
            pattern.bytes.push_back(0);
            pattern.mask.push_back(false);
            pos += (pos + 1 < input.size() && input[pos + 1] == '?') ? 2 : 1;
            continue;
        }
        if (pos + 2 > input.size())
            return failure<Pattern>(Diagnostic::Code::invalid_pattern, "pattern", "truncated byte token");
        unsigned value{};
        auto [end, error] = std::from_chars(input.data() + pos, input.data() + pos + 2, value, 16);
        if (error != std::errc{} || end != input.data() + pos + 2)
            return failure<Pattern>(Diagnostic::Code::invalid_pattern, "pattern", "invalid byte token");
        pattern.bytes.push_back(static_cast<std::uint8_t>(value));
        pattern.mask.push_back(true);
        pos += 2;
    }
    if (!pattern.valid())
        return failure<Pattern>(Diagnostic::Code::invalid_pattern, "pattern", "empty pattern");
    return success(std::move(pattern));
}

std::vector<const std::uint8_t *> find_pattern(AddressRange range, const Pattern &pattern) {
    std::vector<const std::uint8_t *> hits;
    if (!range.valid() || !pattern.valid() || pattern.bytes.size() > static_cast<std::size_t>(range.end - range.begin))
        return hits;
    for (auto p = range.begin; p + pattern.bytes.size() <= range.end; ++p) {
        bool match = true;
        for (std::size_t i = 0; i < pattern.bytes.size(); ++i)
            if (pattern.mask[i] && p[i] != pattern.bytes[i]) {
                match = false;
                break;
            }
        if (match)
            hits.push_back(p);
    }
    return hits;
}

std::vector<const std::uint8_t *> find_bytes(AddressRange range, std::span<const std::uint8_t> bytes) {
    Pattern pattern{std::vector<std::uint8_t>(bytes.begin(), bytes.end()), std::vector<bool>(bytes.size(), true)};
    return find_pattern(range, pattern);
}

std::vector<const std::uint8_t *> find_string(AddressRange range, std::string_view text) {
    return find_bytes(range, std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(text.data()), text.size()));
}

std::vector<const std::uint8_t *> find_pointers(AddressRange range, const void *target) {
    std::vector<const std::uint8_t *> hits;
    if (!range.valid() || !target || range.end - range.begin < static_cast<std::ptrdiff_t>(sizeof(void *)))
        return hits;
    const auto wanted = reinterpret_cast<std::uintptr_t>(target);
    for (auto p = range.begin; p + sizeof(void *) <= range.end; p += sizeof(void *)) {
        auto value = read<const void *>(p);
        if (value && reinterpret_cast<std::uintptr_t>(value.get()) == wanted)
            hits.push_back(p);
    }
    return hits;
}

namespace {

Register register_from_zydis(ZydisRegister reg) {
    switch (reg) {
    case ZYDIS_REGISTER_RCX:
        return Register::rcx;
    case ZYDIS_REGISTER_RDI:
        return Register::rdi;
    case ZYDIS_REGISTER_RSI:
        return Register::rsi;
    default:
        return Register::none;
    }
}

InstructionKind kind_from_zydis(ZydisMnemonic mnemonic) {
    switch (mnemonic) {
    case ZYDIS_MNEMONIC_ADD:
        return InstructionKind::add;
    case ZYDIS_MNEMONIC_CALL:
        return InstructionKind::call;
    case ZYDIS_MNEMONIC_LEA:
        return InstructionKind::lea;
    default:
        return InstructionKind::unknown;
    }
}

} // namespace

Result<Instruction> decode_instruction(const std::uint8_t *address) {
    if (!address)
        return failure<Instruction>(Diagnostic::Code::invalid_argument, "instruction decode", "address is null");

    ZydisDecoder decoder{};
    if (!ZYAN_SUCCESS(ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64)))
        return failure<Instruction>(Diagnostic::Code::invalid_argument, "instruction decode", "x64 decoder initialization failed");

    ZydisDecodedInstruction decoded{};
    ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT]{};
    if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, address, ZYDIS_MAX_INSTRUCTION_LENGTH, &decoded, operands)))
        return failure<Instruction>(Diagnostic::Code::invalid_pattern, "instruction decode", "instruction could not be decoded", reinterpret_cast<std::uintptr_t>(address));

    Instruction result{address, decoded.length, kind_from_zydis(decoded.mnemonic)};
    if (decoded.operand_count_visible > 0 && operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER)
        result.destination = register_from_zydis(operands[0].reg.value);

    for (std::uint8_t index = 0; index < decoded.operand_count_visible; ++index) {
        const auto &operand = operands[index];
        if (operand.type == ZYDIS_OPERAND_TYPE_MEMORY) {
            result.base = register_from_zydis(operand.mem.base);
            if (operand.mem.disp.has_displacement) {
                result.displacement = operand.mem.disp.value;
                result.has_displacement = true;
            }
        }

        if (operand.type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
            result.immediate = operand.imm.value.s;
            result.has_immediate = true;
            if (operand.imm.is_relative)
                result.target = address + decoded.length + operand.imm.value.s;
        }
    }

    return success(result);
}

std::vector<Instruction> disassemble(AddressRange range) {
    std::vector<Instruction> instructions;
    if (!range.valid())
        return instructions;

    for (auto address = range.begin; address < range.end;) {
        auto instruction = decode_instruction(address);
        if (!instruction || instruction->size == 0 || address + instruction->size > range.end)
            break;
        instructions.push_back(instruction.get());
        address += instruction->size;
    }
    return instructions;
}

std::vector<CallSite> call_sites(AddressRange range) {
    std::vector<CallSite> sites;
    const auto instructions = disassemble(range);
    if (instructions.size() < 2)
        return sites;

    for (auto current = instructions.begin(); current != instructions.end(); ++current) {
        if (current->kind != InstructionKind::call || !current->target)
            continue;

        const auto previous_count = current - instructions.begin();
        // Keep enough setup instructions for callers that load a diagnostic
        // string before filling the final register arguments. The console
        // logger call in Scrap Mechanic has the message load eight
        // instructions before the call itself.
        const auto first = current - std::min<std::ptrdiff_t>(16, previous_count);
        CallSite site{*current, previous_count ? *(current - 1) : Instruction{}, {}};
        site.preceding_instructions.assign(first, current);
        sites.push_back(std::move(site));
    }

    return sites;
}

const Instruction *CallSite::find_preceding(InstructionKind kind, Register destination_register, Register base_register) const noexcept {
    for (auto instruction = preceding_instructions.rbegin(); instruction != preceding_instructions.rend(); ++instruction) {
        if (instruction->kind != kind || instruction->destination != destination_register)
            continue;
        if (base_register != Register::none && instruction->base != base_register)
            continue;
        return &*instruction;
    }
    return nullptr;
}

std::vector<RelativeCall> relative_calls(AddressRange range) {
    std::vector<RelativeCall> calls;
    for (const auto &site : call_sites(range)) {
        calls.push_back({site.instruction.address, site.target()});
    }
    return calls;
}

std::vector<RipReference> rip_references(AddressRange range) {
    std::vector<RipReference> references;
    if (!range.valid() || range.end - range.begin < 7)
        return references;
    const auto resolve_unchecked = [](const std::uint8_t *instruction) {
        std::int32_t displacement{};
        std::memcpy(&displacement, instruction + 3, sizeof(displacement));
        return reinterpret_cast<const std::uint8_t *>(reinterpret_cast<std::uintptr_t>(instruction + 7) + displacement);
    };
    for (auto p = range.begin; p + 7 <= range.end; ++p) {
        const bool rip = (p[0] == 0x48 && (p[1] == 0x8B || p[1] == 0x8D || p[1] == 0x89) && (p[2] & 0xC7) == 0x05) ||
                         (p[0] == 0x4C && (p[1] == 0x8B || p[1] == 0x8D) && (p[2] & 0xC7) == 0x05);
        if (rip) {
            references.push_back({p, resolve_unchecked(p)});
        }
    }
    return references;
}

const std::uint8_t *resolve_rip_target(const std::uint8_t *instruction) {
    if (!ProcessMemory::readable(instruction, 7))
        return nullptr;
    std::int32_t displacement{};
    std::memcpy(&displacement, instruction + 3, sizeof(displacement));
    return reinterpret_cast<const std::uint8_t *>(reinterpret_cast<std::uintptr_t>(instruction + 7) + displacement);
}

std::vector<const std::uint8_t *> rip_xrefs(AddressRange text, const void *target, std::size_t target_size) {
    std::vector<const std::uint8_t *> hits;
    const auto wanted = reinterpret_cast<std::uintptr_t>(target);
    for (auto p = text.begin; p + 7 <= text.end; ++p) {
        const bool rip = (p[0] == 0x48 && (p[1] == 0x8B || p[1] == 0x8D || p[1] == 0x89) && (p[2] & 0xC7) == 0x05) ||
                         (p[0] == 0x4C && (p[1] == 0x8B || p[1] == 0x8D) && (p[2] & 0xC7) == 0x05);
        if (!rip)
            continue;
        std::int32_t displacement{};
        std::memcpy(&displacement, p + 3, sizeof(displacement));
        const auto resolved = reinterpret_cast<const std::uint8_t *>(reinterpret_cast<std::uintptr_t>(p + 7) + displacement);
        if (resolved && reinterpret_cast<std::uintptr_t>(resolved) >= wanted && reinterpret_cast<std::uintptr_t>(resolved) < wanted + target_size)
            hits.push_back(p);
    }
    return hits;
}

void *resolve_iat_import(HMODULE image, std::string_view wanted_module, std::string_view wanted_name) {
    auto base = reinterpret_cast<std::uint8_t *>(image);
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
    if (!ProcessMemory::readable(dos, sizeof(*dos)) || dos->e_magic != IMAGE_DOS_SIGNATURE)
        return nullptr;
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
    if (!ProcessMemory::readable(nt, sizeof(*nt)) || nt->Signature != IMAGE_NT_SIGNATURE)
        return nullptr;
    const auto &directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!directory.VirtualAddress || !directory.Size)
        return nullptr;
    auto imports = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR *>(base + directory.VirtualAddress);
    for (; imports->Name; ++imports) {
        auto module_name = reinterpret_cast<const char *>(base + imports->Name);
        std::string normalized(module_name);
        if (normalized.size() < 4 || normalized.substr(normalized.size() - 4) != ".dll")
            normalized += ".dll";
        std::string wanted(wanted_module);
        if (wanted.size() < 4 || wanted.substr(wanted.size() - 4) != ".dll")
            wanted += ".dll";
        if (_stricmp(normalized.c_str(), wanted.c_str()) != 0)
            continue;
        auto names_rva = imports->OriginalFirstThunk ? imports->OriginalFirstThunk : imports->FirstThunk;
        auto names = reinterpret_cast<const IMAGE_THUNK_DATA64 *>(base + names_rva);
        auto slots = reinterpret_cast<void **>(base + imports->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal))
                continue;
            auto by_name = reinterpret_cast<const IMAGE_IMPORT_BY_NAME *>(base + names->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char *>(by_name->Name), std::string(wanted_name).c_str()) == 0)
                return ProcessMemory::readable(slots, sizeof(void *)) ? *slots : nullptr;
        }
    }
    return nullptr;
}

Result<std::pair<const std::uint8_t *, std::size_t>> function_bounds(const std::uint8_t *address, HMODULE module) {
    DWORD64 image_base{};
    auto runtime = RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(address), &image_base, nullptr);
    if (!runtime || image_base != reinterpret_cast<DWORD64>(module))
        return failure<std::pair<const std::uint8_t *, std::size_t>>("function", "unwind metadata not found", reinterpret_cast<std::uintptr_t>(address));
    return success(
        std::make_pair(reinterpret_cast<const std::uint8_t *>(image_base + runtime->BeginAddress), static_cast<std::size_t>(runtime->EndAddress - runtime->BeginAddress)));
}

Result<const std::uint8_t *> containing_function(const std::uint8_t *address, HMODULE module) {
    auto result = function_bounds(address, module);
    if (!result)
        return failure<const std::uint8_t *>(result.error.stage, result.error.message, result.error.address);
    return success(result.get().first);
}

void Resolver::report(const Diagnostic &diagnostic) const {
    if (sink_)
        sink_(diagnostic);
}

Result<const std::uint8_t *> Resolver::unique_pattern(const Pattern &pattern, std::string_view stage) {
    auto hits = find_pattern(sections_.text, pattern);
    if (hits.empty()) {
        auto result = failure<const std::uint8_t *>(Diagnostic::Code::not_found, std::string(stage), "pattern was not found");
        report(result.error);
        return result;
    }
    if (hits.size() != 1) {
        auto result = failure<const std::uint8_t *>(Diagnostic::Code::ambiguous, std::string(stage), "pattern matched multiple locations", hits.size());
        report(result.error);
        return result;
    }
    return success(hits.front());
}

Result<const std::uint8_t *> Resolver::unique_pattern(std::string_view ida_pattern, std::string_view stage) {
    auto parsed = Pattern::parse(ida_pattern);
    if (!parsed) {
        report(parsed.error);
        return failure<const std::uint8_t *>(parsed.error.code, std::string(stage), parsed.error.message);
    }
    return unique_pattern(parsed.get(), stage);
}

Result<const std::uint8_t *> Resolver::unique_string(std::string_view text, std::string_view stage) {
    auto [cached, inserted] = string_cache_.try_emplace(std::string(text));
    if (inserted)
        cached->second = find_string(sections_.rdata, text);
    const auto &hits = cached->second;
    if (hits.empty()) {
        auto result = failure<const std::uint8_t *>(Diagnostic::Code::not_found, std::string(stage), "string was not found");
        report(result.error);
        return result;
    }
    if (hits.size() != 1) {
        auto result = failure<const std::uint8_t *>(Diagnostic::Code::ambiguous, std::string(stage), "string matched multiple locations", hits.size());
        report(result.error);
        return result;
    }
    return success(hits.front());
}

Result<const std::uint8_t *> Resolver::unique_string_xref(std::string_view text, std::string_view stage) {
    auto string_result = unique_string(text, stage);
    if (!string_result)
        return string_result;
    if (!rip_reference_cache_)
        rip_reference_cache_ = mem::rip_references(sections_.text);
    std::vector<const std::uint8_t *> hits;
    const auto wanted = reinterpret_cast<std::uintptr_t>(string_result.get());
    for (const auto &reference : *rip_reference_cache_) {
        const auto target = reinterpret_cast<std::uintptr_t>(reference.target);
        if (target >= wanted && target < wanted + text.size())
            hits.push_back(reference.instruction);
    }
    if (hits.empty()) {
        auto result = failure<const std::uint8_t *>(Diagnostic::Code::not_found, std::string(stage), "string xref was not found");
        report(result.error);
        return result;
    }
    if (hits.size() != 1) {
        auto result = failure<const std::uint8_t *>(Diagnostic::Code::ambiguous, std::string(stage), "string has multiple RIP-relative xrefs", hits.size());
        report(result.error);
        return result;
    }
    return success(hits.front());
}

Result<const std::uint8_t *> Resolver::containing_function(const std::uint8_t *address, std::string_view stage) {
    auto result = mem::containing_function(address, sections_.module);
    if (!result)
        report({std::string(stage), result.error.message, result.error.address});
    return result;
}

namespace hook {

    namespace {
        Result<void> status_result(MH_STATUS status, std::string_view operation, void *target, DiagnosticSink sink) {
            if (status == MH_OK)
                return success();
            Diagnostic diagnostic{Diagnostic::Code::hook, "hook", std::string(operation) + ": " + MH_StatusToString(status), reinterpret_cast<std::uintptr_t>(target)};
            if (sink)
                sink(diagnostic);
            return {false, std::move(diagnostic)};
        }
    } // namespace

    Result<void> initialize(DiagnosticSink sink) {
        return status_result(MH_Initialize(), "initialize", nullptr, std::move(sink));
    }

    Result<void> uninitialize(DiagnosticSink sink) {
        return status_result(MH_Uninitialize(), "uninitialize", nullptr, std::move(sink));
    }

    Result<void> create(void *target, void *detour, void **original, DiagnosticSink sink) {
        return status_result(MH_CreateHook(target, detour, original), "create", target, std::move(sink));
    }

    Result<void> enable(void *target, DiagnosticSink sink) {
        return status_result(MH_EnableHook(target), "enable", target, std::move(sink));
    }

    Result<void> disable(void *target, DiagnosticSink sink) {
        return status_result(MH_DisableHook(target), "disable", target, std::move(sink));
    }

    Result<void> remove(void *target, DiagnosticSink sink) {
        return status_result(MH_RemoveHook(target), "remove", target, std::move(sink));
    }

} // namespace hook

} // namespace mem
