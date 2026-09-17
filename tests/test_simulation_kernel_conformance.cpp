#include "dve/rhi/null_device.hpp"
#include "dve/simulation_kernel_conformance.hpp"

#include <filesystem>
#include <iostream>
#include <stdexcept>

int main() {
    try {
        dve::rhi::NullDevice device;
        const auto fixtures = dve::run_simulation_kernel_conformance(device);
        if (fixtures.size() != 4U) throw std::runtime_error("unexpected fixture count");
        for (const auto& fixture : fixtures) {
            if (fixture.status != dve::ValidationStatus::Skipped)
                throw std::runtime_error("Null RHI readback fixture was not skipped");
            if (fixture.notes.empty())
                throw std::runtime_error("skipped fixture did not explain the execution boundary");
        }
        const auto production = dve::run_simulation_production_pass_conformance(
            device, std::filesystem::path(DVE_TEST_SOURCE_DIR));
        if (production.size() != 3U)
            throw std::runtime_error("unexpected production fixture count");
        for (const auto& fixture : production) {
            if (fixture.status != dve::ValidationStatus::Skipped)
                throw std::runtime_error("Null RHI production fixture was not skipped");
            if (fixture.evidenceClass != "production_pass_specialization_readback")
                throw std::runtime_error("production fixture evidence class was incorrect");
        }
        std::cout << "DVE simulation kernel conformance tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "DVE simulation kernel conformance tests failed: " << exception.what() << '\n';
        return 1;
    }
}
