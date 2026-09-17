#ifndef DVE_MATERIAL_TABLE_HLSLI
#define DVE_MATERIAL_TABLE_HLSLI

#include "material_record.hlsli"

StructuredBuffer<MaterialRecord> gMaterialTable : register(t4);
MaterialRecord LoadMaterialRecord(uint materialId) { return gMaterialTable[materialId]; }

#endif // DVE_MATERIAL_TABLE_HLSLI
