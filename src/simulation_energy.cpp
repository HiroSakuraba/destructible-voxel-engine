#include "dve/simulation_energy.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>
#include <unordered_map>
#include <utility>

namespace dve {
namespace {
void set_error(std::string* error, std::string message) {
    if (error != nullptr) *error = std::move(message);
}

void hash_bytes(std::uint64_t& hash, const void* data, std::size_t size) noexcept {
    constexpr std::uint64_t prime = 1099511628211ULL;
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0U; i < size; ++i) { hash ^= bytes[i]; hash *= prime; }
}

template <class T> void hash_value(std::uint64_t& hash, const T& value) noexcept {
    hash_bytes(hash, &value, sizeof(T));
}

struct Jet2 {
    double value{};
    std::vector<double> gradient;
    std::vector<double> hessian;
};

Jet2 make_constant(double value, std::uint32_t n) {
    return {value, std::vector<double>(n), std::vector<double>(static_cast<std::size_t>(n) * n)};
}

Jet2 unary(const Jet2& x, double value, double first, double second) {
    Jet2 out = make_constant(value, static_cast<std::uint32_t>(x.gradient.size()));
    const std::size_t n = x.gradient.size();
    for (std::size_t i = 0U; i < n; ++i) {
        out.gradient[i] = first * x.gradient[i];
        for (std::size_t j = 0U; j < n; ++j) {
            out.hessian[i * n + j] = first * x.hessian[i * n + j] +
                second * x.gradient[i] * x.gradient[j];
        }
    }
    return out;
}

Jet2 add_jet(const Jet2& a, const Jet2& b, double signB) {
    Jet2 out = make_constant(a.value + signB * b.value,
                             static_cast<std::uint32_t>(a.gradient.size()));
    for (std::size_t i = 0U; i < out.gradient.size(); ++i)
        out.gradient[i] = a.gradient[i] + signB * b.gradient[i];
    for (std::size_t i = 0U; i < out.hessian.size(); ++i)
        out.hessian[i] = a.hessian[i] + signB * b.hessian[i];
    return out;
}

Jet2 multiply_jet(const Jet2& a, const Jet2& b) {
    Jet2 out = make_constant(a.value * b.value,
                             static_cast<std::uint32_t>(a.gradient.size()));
    const std::size_t n = a.gradient.size();
    for (std::size_t i = 0U; i < n; ++i) {
        out.gradient[i] = a.gradient[i] * b.value + b.gradient[i] * a.value;
        for (std::size_t j = 0U; j < n; ++j) {
            out.hessian[i * n + j] = a.hessian[i * n + j] * b.value +
                b.hessian[i * n + j] * a.value + a.gradient[i] * b.gradient[j] +
                b.gradient[i] * a.gradient[j];
        }
    }
    return out;
}

std::array<SimulationEnergyValue, 3> point_expression(
    const SimulationParameterization& p, SimulationEnergyBuilder& builder,
    std::vector<std::uint32_t>& localToGlobal, std::uint32_t& nextLocal) {
    auto var = [&](std::uint32_t global) {
        localToGlobal.push_back(global);
        return builder.variable(nextLocal++);
    };
    if (p.kind == SimulationParameterizationKind::Fixed) {
        return {builder.constant(p.fixedPosition.x), builder.constant(p.fixedPosition.y),
                builder.constant(p.fixedPosition.z)};
    }
    if (p.kind == SimulationParameterizationKind::FreePoint) {
        return {var(p.globalDofBase), var(p.globalDofBase + 1U), var(p.globalDofBase + 2U)};
    }
    std::array<SimulationEnergyValue, 12> q{};
    for (std::uint32_t i = 0U; i < 12U; ++i) q[i] = var(p.globalDofBase + i);
    const auto rx = builder.constant(p.restPosition.x);
    const auto ry = builder.constant(p.restPosition.y);
    const auto rz = builder.constant(p.restPosition.z);
    std::array<SimulationEnergyValue, 3> out{};
    for (std::uint32_t row = 0U; row < 3U; ++row) {
        auto value = q[9U + row];
        value = builder.add(value, builder.multiply(q[row * 3U], rx));
        value = builder.add(value, builder.multiply(q[row * 3U + 1U], ry));
        value = builder.add(value, builder.multiply(q[row * 3U + 2U], rz));
        out[row] = value;
    }
    return out;
}

std::string node_expr(const SimulationEnergyNode& node, std::uint32_t index) {
    auto n = [](std::uint32_t i) { return "n" + std::to_string(i); };
    std::ostringstream out;
    out << std::setprecision(17);
    switch (node.op) {
    case SimulationEnergyOp::Constant: out << node.constant; break;
    case SimulationEnergyOp::Variable: out << "q[" << node.variable << "]"; break;
    case SimulationEnergyOp::Add: out << n(node.a) << " + " << n(node.b); break;
    case SimulationEnergyOp::Subtract: out << n(node.a) << " - " << n(node.b); break;
    case SimulationEnergyOp::Multiply: out << n(node.a) << " * " << n(node.b); break;
    case SimulationEnergyOp::Negate: out << "-" << n(node.a); break;
    case SimulationEnergyOp::Square: out << n(node.a) << " * " << n(node.a); break;
    case SimulationEnergyOp::Log: out << "log(" << n(node.a) << ")"; break;
    case SimulationEnergyOp::Sqrt: out << "sqrt(" << n(node.a) << ")"; break;
    case SimulationEnergyOp::Exp: out << "exp(" << n(node.a) << ")"; break;
    }
    (void)index;
    return out.str();
}
}

#define DVE_ENERGY_BUILDER_BINARY(NAME, OP) \
SimulationEnergyValue SimulationEnergyBuilder::NAME(SimulationEnergyValue a, SimulationEnergyValue b) { \
    nodes_.push_back({SimulationEnergyOp::OP, a.node, b.node, 0U, 0.0}); \
    return {static_cast<std::uint32_t>(nodes_.size() - 1U)}; \
}

SimulationEnergyValue SimulationEnergyBuilder::constant(double value) {
    nodes_.push_back({SimulationEnergyOp::Constant, 0U, 0U, 0U, value});
    return {static_cast<std::uint32_t>(nodes_.size() - 1U)};
}
SimulationEnergyValue SimulationEnergyBuilder::variable(std::uint32_t index) {
    nodes_.push_back({SimulationEnergyOp::Variable, 0U, 0U, index, 0.0});
    return {static_cast<std::uint32_t>(nodes_.size() - 1U)};
}
DVE_ENERGY_BUILDER_BINARY(add, Add)
DVE_ENERGY_BUILDER_BINARY(subtract, Subtract)
DVE_ENERGY_BUILDER_BINARY(multiply, Multiply)
#undef DVE_ENERGY_BUILDER_BINARY
SimulationEnergyValue SimulationEnergyBuilder::negate(SimulationEnergyValue value) {
    nodes_.push_back({SimulationEnergyOp::Negate, value.node});
    return {static_cast<std::uint32_t>(nodes_.size() - 1U)};
}
SimulationEnergyValue SimulationEnergyBuilder::square(SimulationEnergyValue value) {
    nodes_.push_back({SimulationEnergyOp::Square, value.node});
    return {static_cast<std::uint32_t>(nodes_.size() - 1U)};
}
SimulationEnergyValue SimulationEnergyBuilder::log(SimulationEnergyValue value) {
    nodes_.push_back({SimulationEnergyOp::Log, value.node});
    return {static_cast<std::uint32_t>(nodes_.size() - 1U)};
}
SimulationEnergyValue SimulationEnergyBuilder::sqrt(SimulationEnergyValue value) {
    nodes_.push_back({SimulationEnergyOp::Sqrt, value.node});
    return {static_cast<std::uint32_t>(nodes_.size() - 1U)};
}
SimulationEnergyValue SimulationEnergyBuilder::exp(SimulationEnergyValue value) {
    nodes_.push_back({SimulationEnergyOp::Exp, value.node});
    return {static_cast<std::uint32_t>(nodes_.size() - 1U)};
}

bool SimulationEnergyProgram::validate(std::string* error) const {
    if (nodes.empty() || root >= nodes.size()) {
        set_error(error, "simulation energy program has no valid root"); return false;
    }
    for (std::uint32_t i = 0U; i < nodes.size(); ++i) {
        const auto& node = nodes[i];
        auto validChild = [i](std::uint32_t child) { return child < i; };
        switch (node.op) {
        case SimulationEnergyOp::Constant:
            if (!std::isfinite(node.constant)) { set_error(error, "non-finite energy constant"); return false; }
            break;
        case SimulationEnergyOp::Variable:
            if (node.variable >= variableCount) { set_error(error, "energy variable out of range"); return false; }
            break;
        case SimulationEnergyOp::Add: case SimulationEnergyOp::Subtract:
        case SimulationEnergyOp::Multiply:
            if (!validChild(node.a) || !validChild(node.b)) { set_error(error, "energy binary dependency is invalid"); return false; }
            break;
        default:
            if (!validChild(node.a)) { set_error(error, "energy unary dependency is invalid"); return false; }
            break;
        }
    }
    return true;
}

void SimulationEnergyProgram::recompute_hash() noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    hash_value(hash, root); hash_value(hash, variableCount);
    for (const auto& node : nodes) hash_value(hash, node);
    contentHash = hash == 0U ? 1U : hash;
}

SimulationEnergyEvaluation evaluate_simulation_energy(
    const SimulationEnergyProgram& program, std::span<const double> variables) {
    SimulationEnergyEvaluation result;
    result.gradient.resize(program.variableCount);
    result.hessian.resize(static_cast<std::size_t>(program.variableCount) * program.variableCount);
    if (!program.validate(nullptr) || variables.size() != program.variableCount) {
        result.finite = false; return result;
    }
    std::vector<Jet2> values;
    values.reserve(program.nodes.size());
    for (const auto& node : program.nodes) {
        Jet2 value;
        switch (node.op) {
        case SimulationEnergyOp::Constant: value = make_constant(node.constant, program.variableCount); break;
        case SimulationEnergyOp::Variable:
            value = make_constant(variables[node.variable], program.variableCount);
            value.gradient[node.variable] = 1.0; break;
        case SimulationEnergyOp::Add: value = add_jet(values[node.a], values[node.b], 1.0); break;
        case SimulationEnergyOp::Subtract: value = add_jet(values[node.a], values[node.b], -1.0); break;
        case SimulationEnergyOp::Multiply: value = multiply_jet(values[node.a], values[node.b]); break;
        case SimulationEnergyOp::Negate: value = unary(values[node.a], -values[node.a].value, -1.0, 0.0); break;
        case SimulationEnergyOp::Square: value = unary(values[node.a], values[node.a].value * values[node.a].value, 2.0 * values[node.a].value, 2.0); break;
        case SimulationEnergyOp::Log:
            value = unary(values[node.a], std::log(values[node.a].value), 1.0 / values[node.a].value,
                          -1.0 / (values[node.a].value * values[node.a].value)); break;
        case SimulationEnergyOp::Sqrt: {
            const double root = std::sqrt(values[node.a].value);
            value = unary(values[node.a], root, 0.5 / root,
                          -0.25 / (root * values[node.a].value)); break;
        }
        case SimulationEnergyOp::Exp: {
            const double exponential = std::exp(values[node.a].value);
            value = unary(values[node.a], exponential, exponential, exponential); break;
        }
        }
        if (!std::isfinite(value.value) ||
            !std::all_of(value.gradient.begin(), value.gradient.end(), [](double x){ return std::isfinite(x); }) ||
            !std::all_of(value.hessian.begin(), value.hessian.end(), [](double x){ return std::isfinite(x); })) {
            result.finite = false; return result;
        }
        values.push_back(std::move(value));
    }
    const Jet2& root = values[program.root];
    result.energy = root.value; result.gradient = root.gradient; result.hessian = root.hessian;
    return result;
}

SimulationEnergyCompileResult compile_joined_pair_spring_energy(
    const SimulationJoinedPair& pair, double stiffness, double restLength) {
    SimulationEnergyCompileResult result;
    if (!(stiffness >= 0.0) || !(restLength >= 0.0) || !std::isfinite(stiffness) ||
        !std::isfinite(restLength)) {
        result.error = "spring energy parameters are invalid"; return result;
    }
    SimulationEnergyBuilder builder;
    std::uint32_t nextLocal{};
    const auto first = point_expression(pair.first, builder, result.localToGlobalDof, nextLocal);
    const auto second = point_expression(pair.second, builder, result.localToGlobalDof, nextLocal);
    auto squaredDistance = builder.constant(0.0);
    for (std::uint32_t axis = 0U; axis < 3U; ++axis) {
        const auto difference = builder.subtract(first[axis], second[axis]);
        squaredDistance = builder.add(squaredDistance, builder.square(difference));
    }
    const auto distance = builder.sqrt(builder.add(squaredDistance, builder.constant(1.0e-18)));
    const auto extension = builder.subtract(distance, builder.constant(restLength));
    const auto energy = builder.multiply(builder.constant(0.5 * stiffness), builder.square(extension));
    result.program.nodes = builder.nodes();
    result.program.root = energy.node;
    result.program.variableCount = nextLocal;
    result.program.recompute_hash();
    if (!result.program.validate(&result.error)) return result;
    std::vector<std::uint32_t> blocks;
    for (const auto global : result.localToGlobalDof) blocks.push_back(global / 3U);
    std::sort(blocks.begin(), blocks.end()); blocks.erase(std::unique(blocks.begin(), blocks.end()), blocks.end());
    for (const auto a : blocks) for (const auto b : blocks) result.hessianBlocks.push_back({a, b});
    return result;
}

std::string emit_simulation_energy_hlsl(const SimulationEnergyProgram& program,
                                        std::string_view functionName) {
    if (!program.validate(nullptr) || functionName.empty()) return {};
    std::ostringstream out;
    out << "// Deterministic DVE Simulation Energy IR value evaluator.\n";
    out << "float " << functionName << "(StructuredBuffer<float> q) {\n";
    for (std::uint32_t i = 0U; i < program.nodes.size(); ++i)
        out << "    float n" << i << " = " << node_expr(program.nodes[i], i) << ";\n";
    out << "    return n" << program.root << ";\n}\n";
    return out.str();
}

} // namespace dve
