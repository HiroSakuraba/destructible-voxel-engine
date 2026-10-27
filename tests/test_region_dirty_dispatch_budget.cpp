#include "dve/work_debt.hpp"
#include <stdexcept>
#include <cmath>
using namespace dve;
static void require(bool b) { if (!b) throw std::runtime_error("governor frame target regression"); }
int main() {
    WorkDebtGovernor fast(64,1000.0/120.0);
    WorkDebtGovernor slow(64,1000.0/60.0);
    require(std::abs(fast.target_frame_ms()-1000.0/120.0)<1e-9);
    for (int i=0;i<64;++i) {
        const FrameTelemetry frame{10.0,10.0,0,0,0,0};
        (void)fast.update(frame);
        (void)slow.update(frame);
    }
    require(fast.pressure()!=PressureLevel::Normal);
    require(slow.pressure()==PressureLevel::Normal);
    bool invalid=false;
    try { WorkDebtGovernor bad(64,0.0); (void)bad; } catch (const std::invalid_argument&) { invalid=true; }
    require(invalid);
}
