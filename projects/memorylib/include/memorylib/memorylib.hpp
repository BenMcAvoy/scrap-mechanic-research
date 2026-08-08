#pragma once

#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mem {

struct AddressRange {
    const std::uint8_t *begin{};
    const std::uint8_t *end{};

    [[nodiscard]] bool valid() const noexcept {
        return begin != nullptr && end >= begin;
    }

    [[nodiscard]] bool contains(const void *address, std::size_t size = 1) const noexcept;
};

struct ModuleSections {
    HMODULE module{};
    AddressRange image{};
    AddressRange text{};
    AddressRange rdata{};
};

struct Diagnostic {
    enum class Code { invalid_argument, invalid_module, invalid_pattern, not_found, ambiguous, protection, unwind, hook };

    Code code{Code::invalid_argument};
    std::string stage;
    std::string message;
    std::uintptr_t address{};

    Diagnostic() = default;

    Diagnostic(std::string stage_value, std::string message_value, std::uintptr_t address_value = 0)
        : stage(std::move(stage_value)), message(std::move(message_value)), address(address_value) {}

    Diagnostic(Code code_value, std::string stage_value, std::string message_value, std::uintptr_t address_value = 0)
        : code(code_value), stage(std::move(stage_value)), message(std::move(message_value)), address(address_value) {}
};

using DiagnosticSink = std::function<void(const Diagnostic &)>;

template <typename T> struct Result {
    std::optional<T> value;
    Diagnostic error;

    Result() = default;

    Result(std::optional<T> value_value, Diagnostic error_value) : value(std::move(value_value)), error(std::move(error_value)) {}

    template <typename U> Result(const Result<U> &other) : error(other.error) {}

    [[nodiscard]] explicit operator bool() const noexcept {
        return value.has_value();
    }

    [[nodiscard]] bool has_value() const noexcept {
        return value.has_value();
    }

    [[nodiscard]] const T &get() const {
        return *value;
    }

    [[nodiscard]] T &get() {
        return *value;
    }

    [[nodiscard]] const T *operator->() const {
        return &get();
    }

    [[nodiscard]] T *operator->() {
        return &get();
    }

    [[nodiscard]] const T &operator*() const {
        return get();
    }

    [[nodiscard]] T &operator*() {
        return get();
    }

    template <typename U> [[nodiscard]] Result<U> forward_error(std::string stage = {}) const {
        auto diagnostic = error;
        if (!stage.empty())
            diagnostic.stage = std::move(stage);
        return {std::nullopt, std::move(diagnostic)};
    }

    [[nodiscard]] Result context(std::string stage) && {
        if (!value && !stage.empty())
            error.stage = std::move(stage);
        return std::move(*this);
    }
};

template <typename T> Result<T> success(T value) {
    return {std::move(value), {Diagnostic::Code::invalid_argument, {}, {}, 0}};
}

template <typename T> Result<T> failure(Diagnostic::Code code, std::string stage, std::string message, std::uintptr_t address = 0) {
    return {std::nullopt, {code, std::move(stage), std::move(message), address}};
}

template <typename T> Result<T> failure(std::string stage, std::string message, std::uintptr_t address = 0) {
    return failure<T>(Diagnostic::Code::invalid_argument, std::move(stage), std::move(message), address);
}

template <> struct Result<void> {
    bool succeeded{};
    Diagnostic error;

    [[nodiscard]] explicit operator bool() const noexcept {
        return succeeded;
    }

    [[nodiscard]] bool has_value() const noexcept {
        return succeeded;
    }
};

template <typename T> [[nodiscard]] Result<T> exactly_one(const std::vector<T> &matches, std::string stage) {
    if (matches.size() == 1)
        return success(matches.front());
    return failure<T>(matches.empty() ? Diagnostic::Code::not_found : Diagnostic::Code::ambiguous, std::move(stage), matches.empty() ? "no matches" : "multiple matches");
}

template <typename T> [[nodiscard]] Result<T> unique_match(const std::vector<T> &matches, std::string stage, std::string message) {
    if (matches.size() == 1)
        return success(matches.front());
    return failure<T>(matches.empty() ? Diagnostic::Code::not_found : Diagnostic::Code::ambiguous, std::move(stage), std::move(message));
}

template <typename Output, typename Input>
    requires(std::is_pointer_v<Output> && std::is_pointer_v<Input>)
[[nodiscard]] Result<Output> unique_match_as(const std::vector<Input> &matches, std::string stage, std::string message) {
    if (matches.size() == 1) {
        using Pointee = std::remove_const_t<std::remove_pointer_t<Input>>;
        return success(reinterpret_cast<Output>(const_cast<Pointee *>(matches.front())));
    }
    return failure<Output>(matches.empty() ? Diagnostic::Code::not_found : Diagnostic::Code::ambiguous, std::move(stage), std::move(message));
}

template <typename T> void deduplicate(std::vector<T> &values) {
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
}

template <typename T> class Candidates {
  public:

    void add(T candidate) {
        values_.push_back(std::move(candidate));
    }

    [[nodiscard]] bool empty() const noexcept {
        return values_.empty();
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return values_.size();
    }

    [[nodiscard]] const std::vector<T> &values() const noexcept {
        return values_;
    }

    [[nodiscard]] Result<T> exactly_one(std::string stage, std::string message) {
        deduplicate(values_);
        return unique_match(values_, std::move(stage), std::move(message));
    }

  private:

    std::vector<T> values_;
};

template <typename T> [[nodiscard]] Result<T> not_found(std::string stage, std::string message, std::uintptr_t address = 0) {
    return failure<T>(Diagnostic::Code::not_found, std::move(stage), std::move(message), address);
}

template <typename T> [[nodiscard]] Result<T> ambiguous(std::string stage, std::string message, std::uintptr_t address = 0) {
    return failure<T>(Diagnostic::Code::ambiguous, std::move(stage), std::move(message), address);
}

inline Result<void> success() {
    return {true, {Diagnostic::Code::invalid_argument, {}, {}, 0}};
}

inline Result<void> failure(Diagnostic::Code code, std::string stage, std::string message, std::uintptr_t address = 0) {
    return {false, {code, std::move(stage), std::move(message), address}};
}

class ProcessMemory {
  public:

    [[nodiscard]] static bool readable(const void *address, std::size_t size = 1) noexcept;
};

template <typename T>
    requires std::is_trivially_copyable_v<T>
[[nodiscard]] Result<T> read(const void *address) {
    if (!ProcessMemory::readable(address, sizeof(T)))
        return failure<T>(Diagnostic::Code::protection, "memory read", "address is not readable", reinterpret_cast<std::uintptr_t>(address));
    T value{};
    std::memcpy(&value, address, sizeof(T));
    return success(value);
}

template <typename T = const std::uint8_t>
    requires std::is_pointer_v<T *>
[[nodiscard]] Result<T *> read_ptr(const void *address) {
    return read<T *>(address);
}

template <typename T>
    requires std::is_trivially_copyable_v<T>
[[nodiscard]] Result<T> read_at(const void *base, std::size_t offset) {
    if (!base)
        return failure<T>(Diagnostic::Code::invalid_argument, "memory read", "base address is null");
    return read<T>(static_cast<const std::uint8_t *>(base) + offset);
}

[[nodiscard]] HMODULE module_handle(std::wstring_view name = {});
[[nodiscard]] void *export_address(HMODULE module, std::string_view name);

struct CircularListLayout {
    std::size_t sentinel_offset{};
    std::size_t next_offset{};
    std::size_t node_size{};
};

[[nodiscard]] Result<std::vector<const std::uint8_t *>> walk_circular_list(const void *owner, CircularListLayout layout, std::size_t max_nodes = 4096);

class ModuleView {
  public:

    explicit ModuleView(HMODULE module) : module_(module) {}

    [[nodiscard]] Result<ModuleSections> sections(DiagnosticSink sink = {}) const;

    [[nodiscard]] HMODULE module() const noexcept {
        return module_;
    }

  private:

    HMODULE module_{};
};

struct Pattern {
    std::vector<std::uint8_t> bytes;
    std::vector<bool> mask;

    [[nodiscard]] bool valid() const noexcept {
        return !bytes.empty() && bytes.size() == mask.size();
    }

    [[nodiscard]] static Result<Pattern> parse(std::string_view ida_pattern);
};

[[nodiscard]] std::vector<const std::uint8_t *> find_pattern(AddressRange range, const Pattern &pattern);
[[nodiscard]] std::vector<const std::uint8_t *> find_bytes(AddressRange range, std::span<const std::uint8_t> bytes);
[[nodiscard]] std::vector<const std::uint8_t *> find_string(AddressRange range, std::string_view text);
[[nodiscard]] std::vector<const std::uint8_t *> find_pointers(AddressRange range, const void *target);

struct RelativeCall {
    const std::uint8_t *instruction{};
    const std::uint8_t *target{};
};

enum class Register { none, rcx, rdi, rsi };
enum class InstructionKind { unknown, add, call, lea };

struct Instruction {
    const std::uint8_t *address{};
    std::size_t size{};
    InstructionKind kind{InstructionKind::unknown};
    Register destination{Register::none};
    Register base{Register::none};
    std::int64_t displacement{};
    bool has_displacement{};
    std::int64_t immediate{};
    bool has_immediate{};
    const std::uint8_t *target{};

    [[nodiscard]] bool is_add(Register destination_register) const noexcept {
        return kind == InstructionKind::add && destination == destination_register && has_immediate;
    }

    [[nodiscard]] bool is_lea(Register destination_register, Register base_register) const noexcept {
        return kind == InstructionKind::lea && destination == destination_register && base == base_register && has_displacement;
    }
};

struct CallSite {
    Instruction instruction;
    Instruction preceding;
    std::vector<Instruction> preceding_instructions;

    [[nodiscard]] const std::uint8_t *target() const noexcept {
        return instruction.target;
    }

    [[nodiscard]] const Instruction *find_preceding(InstructionKind kind, Register destination, Register base = Register::none) const noexcept;
};

[[nodiscard]] Result<Instruction> decode_instruction(const std::uint8_t *address);
[[nodiscard]] std::vector<Instruction> disassemble(AddressRange range);
[[nodiscard]] std::vector<CallSite> call_sites(AddressRange range);
[[nodiscard]] std::vector<RelativeCall> relative_calls(AddressRange range);

struct RipReference {
    const std::uint8_t *instruction{};
    const std::uint8_t *target{};
};

[[nodiscard]] std::vector<RipReference> rip_references(AddressRange range);
[[nodiscard]] std::vector<const std::uint8_t *> rip_xrefs(AddressRange text, const void *target, std::size_t target_size = 1);
[[nodiscard]] const std::uint8_t *resolve_rip_target(const std::uint8_t *instruction);
[[nodiscard]] void *resolve_iat_import(HMODULE image, std::string_view module_name, std::string_view import_name);

[[nodiscard]] Result<const std::uint8_t *> containing_function(const std::uint8_t *address, HMODULE module);
[[nodiscard]] Result<std::pair<const std::uint8_t *, std::size_t>> function_bounds(const std::uint8_t *address, HMODULE module);

class Resolver {
  public:

    Resolver(ModuleSections sections, DiagnosticSink sink = {}) : sections_(sections), sink_(std::move(sink)) {}

    [[nodiscard]] Result<const std::uint8_t *> unique_pattern(const Pattern &pattern, std::string_view stage);
    [[nodiscard]] Result<const std::uint8_t *> unique_pattern(std::string_view ida_pattern, std::string_view stage);
    [[nodiscard]] Result<const std::uint8_t *> unique_string(std::string_view text, std::string_view stage);
    [[nodiscard]] Result<const std::uint8_t *> unique_string_xref(std::string_view text, std::string_view stage);
    [[nodiscard]] Result<const std::uint8_t *> containing_function(const std::uint8_t *address, std::string_view stage);

    [[nodiscard]] Result<AddressRange> function_range(const std::uint8_t *address, std::string_view stage = "function range") const;

  private:

    void report(const Diagnostic &diagnostic) const;
    ModuleSections sections_;
    DiagnosticSink sink_;
    std::optional<std::vector<RipReference>> rip_reference_cache_;
    std::unordered_map<std::string, std::vector<const std::uint8_t *>> string_cache_;
};

class Scan {
  public:

    static Result<Scan> open(std::wstring_view module_name = {}, DiagnosticSink sink = {});

    explicit Scan(ModuleSections sections, DiagnosticSink sink = {}) : sections_(sections), resolver_(sections_, std::move(sink)) {}

    [[nodiscard]] HMODULE module() const noexcept {
        return sections_.module;
    }

    [[nodiscard]] const ModuleSections &sections() const noexcept {
        return sections_;
    }

    [[nodiscard]] const AddressRange &text() const noexcept {
        return sections_.text;
    }

    [[nodiscard]] const AddressRange &rdata() const noexcept {
        return sections_.rdata;
    }

    [[nodiscard]] Resolver &resolver() noexcept {
        return resolver_;
    }

    [[nodiscard]] Result<const std::uint8_t *> string_xref(std::string_view text, std::string_view stage) {
        return resolver_.unique_string_xref(text, stage);
    }

    [[nodiscard]] Result<const std::uint8_t *> containing_function(const std::uint8_t *address, std::string_view stage) {
        return resolver_.containing_function(address, stage);
    }

    [[nodiscard]] Result<const std::uint8_t *> pattern(std::string_view ida_pattern, std::string_view stage) {
        return resolver_.unique_pattern(ida_pattern, stage);
    }

    [[nodiscard]] Result<std::vector<const std::uint8_t *>> find(const AddressRange &range, std::string_view ida_pattern) const;

    [[nodiscard]] Result<AddressRange> function_range(const std::uint8_t *address, std::string_view stage = "function range") const;

    template <typename T>
        requires std::is_trivially_copyable_v<T>
    [[nodiscard]] Result<T> read(const void *address) const {
        return mem::read<T>(address);
    }

    template <typename T>
        requires std::is_trivially_copyable_v<T>
    [[nodiscard]] Result<T> read_at(const void *base, std::size_t offset) const {
        return mem::read_at<T>(base, offset);
    }

    template <typename T = const std::uint8_t> [[nodiscard]] Result<T *> read_ptr(const void *address) const {
        return mem::read_ptr<T>(address);
    }

  private:

    ModuleSections sections_{};
    Resolver resolver_;
};

namespace hook {

    [[nodiscard]] Result<void> initialize(DiagnosticSink sink = {});
    [[nodiscard]] Result<void> uninitialize(DiagnosticSink sink = {});
    [[nodiscard]] Result<void> create(void *target, void *detour, void **original, DiagnosticSink sink = {});
    [[nodiscard]] Result<void> enable(void *target, DiagnosticSink sink = {});
    [[nodiscard]] Result<void> disable(void *target, DiagnosticSink sink = {});
    [[nodiscard]] Result<void> remove(void *target, DiagnosticSink sink = {});

    template <typename Fn> class Function {
      public:

        [[nodiscard]] Result<void> install(Fn target, Fn detour, DiagnosticSink sink = {}) {
            target_ = target;
            auto created = create(reinterpret_cast<void *>(target), reinterpret_cast<void *>(detour), &trampoline_, sink);
            if (!created)
                return created;
            original_ = reinterpret_cast<Fn>(trampoline_);
            return enable(reinterpret_cast<void *>(target), sink);
        }

        [[nodiscard]] Fn original() const noexcept {
            return original_;
        }

        [[nodiscard]] Fn target() const noexcept {
            return target_;
        }

        [[nodiscard]] Result<void> disable(DiagnosticSink sink = {}) {
            return target_ ? hook::disable(reinterpret_cast<void *>(target_), sink) : success();
        }

        [[nodiscard]] Result<void> remove(DiagnosticSink sink = {}) {
            if (!target_)
                return success();
            auto result = hook::remove(reinterpret_cast<void *>(target_), sink);
            if (result) {
                target_ = nullptr;
                original_ = nullptr;
                trampoline_ = nullptr;
            }
            return result;
        }

      private:

        Fn target_{};
        Fn original_{};
        void *trampoline_{};
    };

} // namespace hook

} // namespace mem

// clang-format off
#define MEM_TRY(name, expression) \
    auto name##_result = (expression); \
    if (!name##_result) \
        return name##_result; \
    auto &&name = *name##_result
// clang-format on
