struct FluoddityParticle
{
    float3 position;
    float padding0;
    float3 velocity;
    float hue;
    float size;
    uint cohort;
    uint age;
    uint padding1;
};

cbuffer FluoddityConstants : register(b0)
{
    uint4 gSimulation;       // particle count, trail resolution, frame, boundary mode
    float4 gIntegration;     // dt, fixed-point scale, maximum speed, deposit strength
    float4 gBoundsAndDrag;   // half extent xyz, drag
    float4 gField;           // gravity, gravity strafe, persistence, diffusion
    uint4 gFlags;            // cohorts, trail mode, sweeps enabled, disable symmetry
    float4 gParameterData[24];
    float4 gRule[30];
};

RWStructuredBuffer<FluoddityParticle> gParticles : register(u1);
RWStructuredBuffer<int4> gAccumulation : register(u2);
RWTexture3D<float4> gTrailRead : register(u3);

float3 safe_normalize(float3 value, float3 fallback)
{
    const float lengthSquared = dot(value, value);
    return lengthSquared > 1.0e-10 ? value * rsqrt(lengthSquared) : fallback;
}

int3 trail_coordinate(float3 position)
{
    const float3 normalized = saturate(position / (2.0 * gBoundsAndDrag.xyz) + 0.5);
    return clamp(int3(normalized * float(gSimulation.y)), 0,
                 int(gSimulation.y) - 1);
}

uint flat_index(int3 coordinate)
{
    return uint(coordinate.x) + gSimulation.y *
           (uint(coordinate.y) + gSimulation.y * uint(coordinate.z));
}


float parameter_value(uint index, float3 position, uint cohort)
{
    const float4 primary = gParameterData[index * 2];
    const float4 modulation = gParameterData[index * 2 + 1];
    float value = primary.x;
    if (gFlags.z != 0)
    {
        const float3 normalized = saturate(position / (2.0 * gBoundsAndDrag.xyz) + 0.5);
        const float cohort01 = gFlags.x > 1 ? float(cohort) / float(gFlags.x - 1) : 0.0;
        value += primary.w * (normalized.x * 2.0 - 1.0);
        value += modulation.x * (normalized.y * 2.0 - 1.0);
        value += modulation.y * (cohort01 * 2.0 - 1.0);
    }
    if (modulation.z != 0.0)
    {
        const uint hash = (cohort * 747796405u + gSimulation.z * 2891336453u + index * 277803737u);
        const float jitter = (float(hash & 0xffffu) / 32767.5 - 1.0) * modulation.z;
        value += jitter;
    }
    return clamp(value, primary.y, primary.z);
}

float3 evaluate_rule(float3 localVelocity, float3 sensedTrail)
{
    const float inputValues[6] = {
        localVelocity.x, localVelocity.y, localVelocity.z,
        sensedTrail.x, sensedTrail.y, sensedTrail.z
    };
    float outputValues[6] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
    [unroll]
    for (uint center = 0; center < 10; ++center)
    {
        const float4 frequency = gRule[center * 3 + 0];
        const float4 amplitude = gRule[center * 3 + 1];
        const float4 extension = gRule[center * 3 + 2];
        const float phase = dot(frequency, float4(inputValues[0], inputValues[1],
                                                  inputValues[2], inputValues[3])) +
                            extension.x * inputValues[4] + extension.y * inputValues[5];
        const float wave = sin(phase);
        outputValues[0] += amplitude.x * wave;
        outputValues[1] += amplitude.y * wave;
        outputValues[2] += amplitude.z * wave;
        outputValues[3] += amplitude.w * wave;
        outputValues[4] += extension.z * wave;
        outputValues[5] += extension.w * wave;
    }
    return float3(outputValues[0] + outputValues[3],
                  outputValues[1] + outputValues[4],
                  outputValues[2] + outputValues[5]);
}

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint particleIndex = dispatchThreadId.x;
    if (particleIndex >= gSimulation.x)
        return;

    FluoddityParticle particle = gParticles[particleIndex];
    const float3 forward = safe_normalize(particle.velocity, float3(0.0, 1.0, 0.0));
    const float3 reference = abs(forward.y) < 0.95 ? float3(0.0, 1.0, 0.0)
                                                   : float3(1.0, 0.0, 0.0);
    const float3 lateral = safe_normalize(cross(forward, reference), float3(1.0, 0.0, 0.0));
    const float3 up = cross(lateral, forward);
    const float sensorGain = parameter_value(0, particle.position, particle.cohort);
    const float sensorDistance = parameter_value(2, particle.position, particle.cohort);
    const float axialForce = parameter_value(6, particle.position, particle.cohort);
    const float lateralForce = parameter_value(7, particle.position, particle.cohort);
    const float strafePower = parameter_value(8, particle.position, particle.cohort);
    const float drag = parameter_value(5, particle.position, particle.cohort);
    const int3 senseCoordinate = trail_coordinate(
        particle.position + forward * sensorDistance);
    const float3 sensedTrail = gTrailRead[senseCoordinate].xyz * sensorGain;
    const float3 localVelocity = float3(dot(particle.velocity, lateral),
                                        dot(particle.velocity, up),
                                        dot(particle.velocity, forward));
    const float3 ruleForce = evaluate_rule(localVelocity, sensedTrail);
    float3 acceleration = forward * (axialForce + ruleForce.z) +
                          lateral * (lateralForce + ruleForce.x) +
                          up * (strafePower + ruleForce.y) +
                          float3(0.0, -gField.x, 0.0) +
                          lateral * gField.y;
    particle.velocity += acceleration * gIntegration.x;
    particle.velocity *= exp(-max(0.0, drag) * gIntegration.x);
    const float speed = length(particle.velocity);
    if (speed > gIntegration.z && speed > 0.0)
        particle.velocity *= gIntegration.z / speed;
    particle.position += particle.velocity * gIntegration.x;

    [unroll]
    for (uint axis = 0; axis < 3; ++axis)
    {
        if (gSimulation.w == 2)
        {
            const float width = 2.0 * gBoundsAndDrag[axis];
            if (particle.position[axis] < -gBoundsAndDrag[axis])
                particle.position[axis] += width;
            if (particle.position[axis] > gBoundsAndDrag[axis])
                particle.position[axis] -= width;
        }
        else if (abs(particle.position[axis]) > gBoundsAndDrag[axis])
        {
            particle.position[axis] = clamp(particle.position[axis],
                                            -gBoundsAndDrag[axis],
                                             gBoundsAndDrag[axis]);
            particle.velocity[axis] *= -0.5;
        }
    }

    const uint voxel = flat_index(trail_coordinate(particle.position));
    const float density = gFlags.y == 0 ? 0.0 : 1.0;
    const int4 contribution = int4(round(float4(particle.velocity * gIntegration.w,
                                                density * gIntegration.w) *
                                         gIntegration.y));
    int ignored;
    InterlockedAdd(gAccumulation[voxel].x, contribution.x, ignored);
    InterlockedAdd(gAccumulation[voxel].y, contribution.y, ignored);
    InterlockedAdd(gAccumulation[voxel].z, contribution.z, ignored);
    InterlockedAdd(gAccumulation[voxel].w, contribution.w, ignored);
    particle.age += 1;
    gParticles[particleIndex] = particle;
}
