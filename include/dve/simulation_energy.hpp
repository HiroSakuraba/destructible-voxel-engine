#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "dve/transform.hpp"

namespace dve {

enum class SimulationEnergyOp : std::uint8_t {
    Constant,
    Variable,
    Add,
    Subtract,
    Multiply,
    Negate,
    Square,
    Log,
    Sqrt,
    Exp,
};

struct SimulationEnergyNode {
    SimulationEnergyOp op{SimulationEnergyOp::Constant};
    std::uint32_t a{};
    std::uint32_t b{};
    std::uint32_t variable{};
    double constant{};
};

struct SimulationEnergyValue { std::uint32_t node{}; };

class SimulationEnergyBuilder {
public:
    [[nodiscard]] SimulationEnergyValue constant(double value);
    [[nodiscard]] SimulationEnergyValue variable(std::uint32_t index);
    [[nodiscard]] SimulationEnergyValue add(SimulationEnergyValue a, SimulationEnergyValue b);
    [[nodiscard]] SimulationEnergyValue subtract(SimulationEnergyValue a, SimulationEnergyValue b);
    [[nodiscard]] SimulationEnergyValue multiply(SimulationEnergyValue a, SimulationEnergyValue b);
    [[nodiscard]] SimulationEnergyValue negate(SimulationEnergyValue value);
    [[nodiscard]] SimulationEnergyValue square(SimulationEnergyValue value);
    [[nodiscard]] SimulationEnergyValue log(SimulationEnergyValue value);
    [[nodiscard]] SimulationEnergyValue sqrt(SimulationEnergyValue value);
    [[nodiscard]] SimulationEnergyValue exp(SimulationEnergyValue value);

    [[nodiscard]] const std::vector<SimulationEnergyNode>& nodes() const noexcept { return nodes_; }
private:
    std::vector<SimulationEnergyNode> nodes_;
};

struct SimulationEnergyProgram {
    std::vector<SimulationEnergyNode> nodes;
    std::uint32_t root{};
    std::uint32_t variableCount{};
    std::uint64_t contentHash{};

    [[nodiscard]] bool validate(std::string* error = nullptr) const;
    void recompute_hash() noexcept;
};

struct SimulationEnergyEvaluation {
    double energy{};
    std::vector<double> gradient;
    std::vector<double> hessian; // row-major variableCount x variableCount
    bool finite{true};
};

[[nodiscard]] SimulationEnergyEvaluation evaluate_simulation_energy(
    const SimulationEnergyProgram& program, std::span<const double> variables);

enum class SimulationParameterizationKind : std::uint8_t { Fixed, FreePoint, AffinePoint };

struct SimulationParameterization {
    SimulationParameterizationKind kind{SimulationParameterizationKind::Fixed};
    Float3 restPosition{};
    Float3 fixedPosition{};
    std::uint32_t globalDofBase{};
};

struct SimulationJoinedPair {
    SimulationParameterization first;
    SimulationParameterization second;
};

struct SimulationEnergyCompileResult {
    SimulationEnergyProgram program;
    std::vector<std::uint32_t> localToGlobalDof;
    std::vector<std::array<std::uint32_t, 2>> hessianBlocks;
    std::string error;

    [[nodiscard]] explicit operator bool() const noexcept { return error.empty(); }
};

// YASPS-inspired JOIN/UNION reference compiler: JOIN supplies the pair relation;
// each endpoint is UNIONed across Fixed, FreePoint and AffinePoint parameterizations.
[[nodiscard]] SimulationEnergyCompileResult compile_joined_pair_spring_energy(
    const SimulationJoinedPair& pair, double stiffness, double restLength);

[[nodiscard]] std::string emit_simulation_energy_hlsl(const SimulationEnergyProgram& program,
                                                       std::string_view functionName);

} // namespace dve
