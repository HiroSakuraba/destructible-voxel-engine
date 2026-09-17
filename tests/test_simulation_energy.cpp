#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <vector>

#include "dve/simulation_energy.hpp"

namespace {
void require(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
void close(double a, double b, double tolerance, const char* message) {
    if (std::abs(a - b) > tolerance) {
        std::cerr << "FAIL: " << message << " (" << a << " vs " << b << ")\n";
        std::exit(1);
    }
}
}

int main() {
    dve::SimulationJoinedPair pair;
    pair.first.kind = dve::SimulationParameterizationKind::FreePoint;
    pair.first.globalDofBase = 30U;
    pair.second.kind = dve::SimulationParameterizationKind::AffinePoint;
    pair.second.globalDofBase = 60U;
    pair.second.restPosition = {1.0F, 0.0F, 0.0F};
    const auto compiled = dve::compile_joined_pair_spring_energy(pair, 4.0, 1.0);
    require(static_cast<bool>(compiled), "mixed JOIN/UNION energy should compile");
    require(compiled.program.variableCount == 15U, "free plus affine point should use 15 local dofs");
    require(!compiled.hessianBlocks.empty(), "sparsity blocks should be inferred");

    std::vector<double> q(compiled.program.variableCount, 0.0);
    q[0] = 0.0; q[1] = 0.0; q[2] = 0.0;
    // affine identity and zero translation
    q[3] = 1.0; q[7] = 1.0; q[11] = 1.0;
    auto evaluation = dve::evaluate_simulation_energy(compiled.program, q);
    require(evaluation.finite, "evaluation should remain finite");
    close(evaluation.energy, 0.0, 1.0e-10, "rest configuration energy");

    q[0] = -1.0;
    evaluation = dve::evaluate_simulation_energy(compiled.program, q);
    close(evaluation.energy, 2.0, 1.0e-8, "stretched spring energy");
    close(evaluation.gradient[0], -4.0, 1.0e-6, "analytic gradient");

    const double epsilon = 1.0e-5;
    auto qp = q; auto qm = q;
    qp[0] += epsilon; qm[0] -= epsilon;
    const double finiteDifference = (dve::evaluate_simulation_energy(compiled.program, qp).energy -
                                     dve::evaluate_simulation_energy(compiled.program, qm).energy) /
                                    (2.0 * epsilon);
    close(evaluation.gradient[0], finiteDifference, 1.0e-5, "gradient finite difference");
    const auto hlsl = dve::emit_simulation_energy_hlsl(compiled.program, "spring_energy");
    require(hlsl.find("spring_energy") != std::string::npos, "HLSL value evaluator should emit");
    std::cout << "simulation energy tests passed\n";
}
