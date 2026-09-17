-- DVE v1.31 Material Parameter Collection, Clear Coat, Two-Sided Foliage, and Material Layers.
-- Run from the gameplay script host. Numeric globals remain ordinary script globals unless a
-- material explicitly binds to them or they are canonical collection names.

local rustMaster = world.create_master_material("RustLayer", {
    shading_model = "standard_pbr",
    scalars = {
        {name="Metallic", default=0.08, min=0.0, max=1.0},
        {name="Roughness", default=0.92, min=0.0, max=1.0},
        {name="Specular", default=0.32, min=0.0, max=1.0}
    },
    vectors = {
        {name="BaseColor", default={0.58, 0.10, 0.025, 1.0}}
    }
})

world.create_material_instance({
    name="Rust", material_id=10, master=rustMaster
})

local paintMaster = world.create_master_material("PaintedClearCoat", {
    shading_model = "clear_coat",
    scalars = {
        {name="Metallic", default=0.72, min=0.0, max=1.0},
        {name="Roughness", default=0.34, min=0.0, max=1.0},
        {name="Specular", default=0.50, min=0.0, max=1.0},
        {name="ClearCoat", default=0.90, min=0.0, max=1.0},
        {name="ClearCoatRoughness", default=0.08, min=0.0, max=1.0}
    },
    vectors = {
        {name="BaseColor", default={0.12, 0.32, 0.82, 1.0}}
    },
    global_scalars = {
        {parameter="Roughness", global="RoughnessScale", combine="multiply"}
    },
    global_vectors = {
        {parameter="BaseColor", global="TimeOfDayTint", combine="multiply"}
    }
})

world.create_material_instance({
    name="RustOverPaint",
    material_id=11,
    master=paintMaster,
    layers={{material_id=10, weight=0.35, blend="lerp"}}
})

local foliageMaster = world.create_master_material("TwoSidedLeaf", {
    shading_model = "two_sided_foliage",
    blend_mode = "masked",
    scalars = {
        {name="Roughness", default=0.68, min=0.0, max=1.0},
        {name="Specular", default=0.32, min=0.0, max=1.0},
        {name="FoliageTransmittance", default=0.72, min=0.0, max=1.0},
        {name="FoliageWrap", default=0.38, min=0.0, max=1.0}
    },
    vectors = {
        {name="BaseColor", default={0.13, 0.42, 0.08, 1.0}},
        {name="FoliageColor", default={0.42, 0.78, 0.20, 1.0}}
    },
    global_vectors = {
        {parameter="BaseColor", global="TimeOfDayTint", combine="multiply"}
    }
})

world.create_material_instance({
    name="WindLeaf", material_id=12, master=foliageMaster
})

-- Canonical collection values are read directly by the shading pass.
world.set_global("TimeSeconds", 42.0)
world.set_global("WindStrength", 0.65)
world.set_global("Wetness", 0.20)
world.set_global_vector("WindDirection", 0.35, 0.0, 0.94, 0.0)
world.set_global_vector("TimeOfDayTint", 1.0, 0.72, 0.48, 1.0)

-- This custom collection value is read only by masters that explicitly bind it.
world.set_global("RoughnessScale", 0.75)

-- Uniform runtime layer weights are one-based in Lua.
world.set_material_layer_weight(11, 1, 0.35)
