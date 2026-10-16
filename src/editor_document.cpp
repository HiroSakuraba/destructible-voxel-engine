#include "dve/voxel_metric.hpp"
#include "dve/editor_document.hpp"

#include <unordered_map>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <system_error>

#include "dve/dvox.hpp"
#include "dve/editor_materials.hpp"
#include "dve/editor_subdivision.hpp"

namespace dve::editor {
namespace {

std::string quote_text(std::string_view text) {
    std::ostringstream stream;
    stream << std::quoted(std::string(text));
    return stream.str();
}

std::filesystem::path make_revision_path(const std::filesystem::path& manifest, std::uint64_t revision) {
    const std::string stem = manifest.stem().string();
    return manifest.parent_path() / (stem + ".objects.r" + std::to_string(revision));
}

VoxelMaterialDefinition default_material(std::size_t index) {
    VoxelMaterialDefinition material;
    material.name = index == 0 ? "Air" : "Editor Material " + std::to_string(index);
    material.baseColor = index == 0 ? Float4{0, 0, 0, 0} : Float4{0.65F, 0.68F, 0.72F, 1.0F};
    material.densityKilogramsPerCubicMeter = index == 0 ? 0.0F : 1000.0F;
    material.structural = index != 0;
    return material;
}

void write_component_value(std::ostream& output, const ComponentValue& value) {
    std::visit([&](const auto& item) {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, bool>) output << " b " << item;
        else if constexpr (std::is_same_v<T, std::int64_t>) output << " i " << item;
        else if constexpr (std::is_same_v<T, double>) output << " n " << std::setprecision(17) << item;
        else if constexpr (std::is_same_v<T, std::string>) output << " s " << std::quoted(item);
        else if constexpr (std::is_same_v<T, Float3>)
            output << " v " << std::setprecision(9) << item.x << ' ' << item.y << ' ' << item.z;
        else if constexpr (std::is_same_v<T, Quaternion>)
            output << " q " << std::setprecision(9) << item.x << ' ' << item.y << ' ' << item.z << ' ' << item.w;
    }, value);
}

ComponentValue read_component_value(std::istream& input) {
    char type{};
    input >> type;
    switch (type) {
        case 'b': { bool value{}; input >> value; return value; }
        case 'i': { std::int64_t value{}; input >> value; return value; }
        case 'n': { double value{}; input >> value; return value; }
        case 's': { std::string value; input >> std::quoted(value); return value; }
        case 'v': { Float3 value{}; input >> value.x >> value.y >> value.z; return value; }
        case 'q': { Quaternion value{}; input >> value.x >> value.y >> value.z >> value.w; return value; }
        default: throw std::runtime_error("unsupported component value type");
    }
}

} // namespace

EditorObject::EditorObject() : voxels(std::make_unique<VoxelObject>(1)) {}
EditorObject::EditorObject(EditorObjectId objectId, std::string objectName)
    : id(objectId), name(std::move(objectName)), voxels(std::make_unique<VoxelObject>(objectId)) {}

std::unique_ptr<VoxelObject> clone_voxel_object(const VoxelObject& source) {
    auto copy = std::make_unique<VoxelObject>(source.id());
    copy->reserve_bricks(source.brick_count());
    // Copy whole bricks (one encode each) instead of one set_voxel per voxel. Bricks
    // with no occupied voxels are skipped, as before, so the copy holds the same set
    // of bricks; their generations are carried over.
    for (const auto& entry : source.bricks()) {
        if (entry.second.occupancy().none()) continue;
        if (!copy->replace_brick(source.snapshot_brick(entry.first)))
            throw std::runtime_error("could not clone voxel brick");
    }
    return copy;
}

EditorObject clone_editor_object(const EditorObject& source) {
    EditorObject cloned(source.id, source.name);
    cloned.parent = source.parent;
    cloned.transform = source.transform;
    cloned.attachment = source.attachment;
    cloned.flags = source.flags;
    cloned.sourceAsset = source.sourceAsset;
    cloned.importRecipe = source.importRecipe;
    cloned.textFontAsset = source.textFontAsset;
    cloned.voxelSizeMeters = source.voxelSizeMeters;
    cloned.voxelScale = source.voxelScale;
    cloned.voxelMaterials = source.voxelMaterials;
    cloned.polygon = source.polygon;
    cloned.subdivision = source.subdivision;
    cloned.modelingTopology = source.modelingTopology;cloned.modelingNextId=source.modelingNextId;
    cloned.voxels = clone_voxel_object(*source.voxels);
    cloned.text3d = source.text3d;
    cloned.gaborVolume = source.gaborVolume;
    cloned.anchors = source.anchors;
    cloned.components = source.components;
    cloned.tags = source.tags;
    cloned.groups = source.groups;
    cloned.layer = source.layer;
    cloned.prefabLink = source.prefabLink;
    return cloned;
}

EditorDocument clone_editor_document(const EditorDocument& source) {
    EditorDocument copy(source.name());
    for (const auto& [id, object] : source.objects()) {
        (void)id;
        copy.add_object(clone_editor_object(object));
    }
    copy.path_ = source.path_;
    copy.revision_ = source.revision_;
    copy.nextPrefabInstanceId_ = source.nextPrefabInstanceId_;
    copy.selectionSets_ = source.selectionSets_;
    copy.selectionLineage_ = source.selectionLineage_;
    copy.dirty_ = source.dirty_;
    return copy;
}

EditorDocument::EditorDocument(std::string name) : name_(std::move(name)) {}

void EditorDocument::record_selection_lineage(EditorObjectId mesh, const std::map<ModelingId, std::set<ModelingId>>& descendants) {
    auto& lineage = selectionLineage_[mesh];
    for (const auto& [id, next] : descendants) {
        if (next.size() == 1U && *next.begin() == id) continue;
        lineage[id].insert(next.begin(), next.end());
    }
    if (lineage.empty()) selectionLineage_.erase(mesh);
}
void EditorDocument::set_name(std::string name) { name_ = std::move(name); dirty_ = true; }
EditorObjectId EditorDocument::allocate_object_id() noexcept {
    while (objects_.contains(nextObjectId_) || nextObjectId_ == 0) ++nextObjectId_;
    return nextObjectId_++;
}

EditorObject& EditorDocument::add_object(EditorObject object) {
    if (object.id == 0) object.id = allocate_object_id();
    if (objects_.contains(object.id)) throw std::invalid_argument("duplicate editor object id");
    if (!object.voxels || object.voxels->id() != object.id) {
        auto replacement = std::make_unique<VoxelObject>(object.id);
        if (object.voxels) {
            for (const auto& entry : object.voxels->bricks()) {
                const BrickKey key = entry.first;
                const Brick& brick = entry.second;
                brick.occupancy().for_each_set([&](std::uint16_t index) {
                    replacement->set_voxel(global_from_local(key, local_from_index_unchecked(index)), brick.material(index));
                });
            }
        }
        object.voxels = std::move(replacement);
    }
    if (object.polygon) {
        object.polygon->objectId=object.id;
        object.polygon->contentHash=polygon_asset_content_hash(*object.polygon);
    }
    nextObjectId_ = std::max(nextObjectId_, object.id + 1);
    const auto [it, inserted] = objects_.emplace(object.id, std::move(object));
    if (!inserted) throw std::runtime_error("failed to insert editor object");
    dirty_ = true;
    return it->second;
}

bool EditorDocument::remove_object(EditorObjectId id) {
    if (!objects_.contains(id)) return false;
    std::vector<EditorObjectId> remove{id};
    for (std::size_t cursor = 0; cursor < remove.size(); ++cursor) {
        for (const auto& [childId, object] : objects_) {
            if (object.parent == remove[cursor]) remove.push_back(childId);
        }
    }
    for (EditorObjectId objectId : remove) objects_.erase(objectId);
    dirty_ = true;
    return true;
}

bool EditorDocument::reparent(EditorObjectId id, std::optional<EditorObjectId> parent) {
    EditorObject* object = find_object(id);
    if (!object || parent == id || (parent && !objects_.contains(*parent))) return false;
    for (auto cursor = parent; cursor;) {
        if (*cursor == id) return false;
        const EditorObject* ancestor = find_object(*cursor);
        cursor = ancestor ? ancestor->parent : std::nullopt;
    }
    const auto world = world_transform(id);
    object->parent = parent;
    if (object->attachment) {
        if (parent) {
            const auto parentWorld = world_transform(*parent);
            if (!world || !parentWorld) return false;
            object->attachment->localTransform = attachment_local_from_world(
                *parentWorld, *world,
                object->attachment->inheritPosition, object->attachment->inheritRotation);
        } else {
            object->attachment.reset();
            if (world) object->transform = *world;
        }
    }
    dirty_ = true;
    refresh_attachment_world_transforms();
    return true;
}

EditorObject* EditorDocument::find_object(EditorObjectId id) noexcept {
    const auto it = objects_.find(id);
    return it == objects_.end() ? nullptr : &it->second;
}
const EditorObject* EditorDocument::find_object(EditorObjectId id) const noexcept {
    const auto it = objects_.find(id);
    return it == objects_.end() ? nullptr : &it->second;
}

std::vector<EditorObjectId> EditorDocument::root_objects() const {
    std::vector<EditorObjectId> result;
    for (const auto& [id, object] : objects_) if (!object.parent) result.push_back(id);
    return result;
}
std::vector<EditorObjectId> EditorDocument::children_of(EditorObjectId parent) const {
    std::vector<EditorObjectId> result;
    for (const auto& [id, object] : objects_) if (object.parent == parent) result.push_back(id);
    return result;
}

std::vector<EditorObjectId> EditorDocument::hierarchy_preorder() const {
    // objects_ is id-ordered, so bucketing children in iteration order keeps siblings
    // in id order, matching children_of(). Parents that do not exist are skipped just
    // like the recursive walk (such children are unreachable from any root).
    std::unordered_map<EditorObjectId, std::vector<EditorObjectId>> children;
    children.reserve(objects_.size());
    std::vector<EditorObjectId> roots;
    for (const auto& [id, object] : objects_) {
        if (object.parent) children[*object.parent].push_back(id);
        else roots.push_back(id);
    }
    std::vector<EditorObjectId> result;
    result.reserve(objects_.size());
    std::vector<EditorObjectId> stack;
    for (auto root = roots.rbegin(); root != roots.rend(); ++root) stack.push_back(*root);
    while (!stack.empty()) {
        const EditorObjectId id = stack.back();
        stack.pop_back();
        result.push_back(id);
        // Guard against malformed cycles (validate() rejects them, but never loop forever).
        if (result.size() > objects_.size()) break;
        const auto it = children.find(id);
        if (it == children.end()) continue;
        for (auto child = it->second.rbegin(); child != it->second.rend(); ++child) stack.push_back(*child);
    }
    return result;
}

bool EditorDocument::validate(std::string* error) const {
    auto fail = [&](std::string message) { if (error) *error = std::move(message); return false; };
    for (const auto& [id, object] : objects_) {
        if (id == 0 || id != object.id) return fail("invalid object identifier");
        if (!object.voxels || object.voxels->id() != id || !object.voxels->validate()) return fail("invalid voxel object");
        if (object.text3d) {
            if (object.text3d->objectId != id) return fail("3D text object identifier drift");
            std::string textError;
            if (!validate_text3d_asset(*object.text3d, &textError)) return fail("invalid 3D text asset: " + textError);
            if (object.voxels->occupied_voxel_count() != 0U) return fail("3D text object also contains voxel geometry");
        }
        if (object.gaborVolume) {
            std::string gaborError;
            if (!object.gaborVolume->validate(&gaborError)) return fail("invalid Gabor volume asset: " + gaborError);
            if (object.voxels->occupied_voxel_count() != 0U) return fail("Gabor volume object also contains voxel geometry");
        }
        if (object.text3d && object.gaborVolume) return fail("object contains multiple geometry kinds");
        if (object.parent && !objects_.contains(*object.parent)) return fail("missing parent object");
        if (object.attachment && !object.parent) return fail("attachment has no parent object");
        if (object.attachment) {
            std::string attachmentError;
            if (!object.attachment->validate(&attachmentError)) return fail("invalid attachment: " + attachmentError);
        }
        const auto normalizedTags = normalize_membership_values(object.tags);
        const auto normalizedGroups = normalize_membership_values(object.groups);
        if (normalizedTags != object.tags) return fail("object tags are not normalized");
        if (normalizedGroups != object.groups) return fail("object groups are not normalized");
        if (object.layer > 63U) return fail("object layer is out of range");
        if (const Component* membership = find_component_by_type(
                std::span<const Component>(object.components), "dve.membership")) {
            const auto tagsIt = membership->properties.find("tags");
            const auto groupsIt = membership->properties.find("groups");
            const auto layerIt = membership->properties.find("layer");
            if (tagsIt == membership->properties.end() || groupsIt == membership->properties.end() ||
                layerIt == membership->properties.end()) return fail("membership component is incomplete");
            const auto* tags = std::get_if<std::string>(&tagsIt->second);
            const auto* groups = std::get_if<std::string>(&groupsIt->second);
            const auto* layer = std::get_if<std::int64_t>(&layerIt->second);
            if (!tags || !groups || !layer || parse_membership_values(*tags) != object.tags ||
                parse_membership_values(*groups) != object.groups || *layer != static_cast<std::int64_t>(object.layer))
                return fail("membership component and object membership fields disagree");
        }
        static const ComponentTypeRegistry componentRegistry = ComponentTypeRegistry::make_default();
        std::string componentError;
        if (!validate_components(object.components, &componentRegistry, &componentError))
            return fail("invalid component set: " + componentError);
        if (object.prefabLink) {
            std::string prefabError;
            if (!object.prefabLink->validate(&prefabError)) return fail("invalid prefab link: " + prefabError);
        }
        if (!object.voxelMaterials.empty()) {
            if (object.polygon || object.text3d || object.gaborVolume) return fail("palette belongs to non-voxel geometry");
            EditorMaterialLibrary paletteValidator;
            for (const auto& [materialId,definition] : object.voxelMaterials) {
                std::string materialError;
                if (materialId == kAirMaterial || !paletteValidator.upsert({materialId,definition,false},&materialError))
                    return fail("invalid voxel palette: "+materialError);
            }
        }
        if (object.subdivision) {
            if (!object.polygon) return fail("subdivision object has no control cage");
            if (object.polygon->indices.size()/3>kMaxSubdivisionControlTriangles || object.polygon->vertices.size()>100000)
                return fail("subdivision control cage exceeds its budget");
            if (object.subdivision->level==0) {
                const auto evaluated=evaluate_subdivision(*object.polygon,*object.subdivision);
                if (!evaluated.mesh) return fail(evaluated.error);
            }
            (void)editor_surface_mesh(object);
            if (object.subdivision->level>kMaxSubdivisionLevel ||
                (object.subdivisionCache && !object.subdivisionCache->mesh))
                return fail(object.subdivisionCache ? object.subdivisionCache->error : "invalid subdivision level");
        }
        if (object.modelingTopology) {
            if(!object.polygon || object.modelingNextId<object.modelingTopology->nextId ||
                !validate_modeling_topology(*object.polygon,object.subdivision?&*object.subdivision:nullptr,*object.modelingTopology,error)) return fail("Invalid persistent modeling topology");
        }
        if (object.polygon) {
            const auto valid=validate_polygon_asset(*object.polygon);
            if (!valid || object.polygon->objectId!=id) return fail("invalid polygon object");
            if (object.text3d || object.gaborVolume || object.voxels->occupied_voxel_count()!=0)
                return fail("object has conflicting geometry");
        }
        if (!valid_voxel_metric(object.voxelScale) || !std::isfinite(object.voxelSizeMeters) || !(object.voxelSizeMeters > 0.0F)) return fail("invalid voxel size");
        for (const Int3 anchor : object.anchors) {
            if (!object.voxels->occupied_at(anchor)) return fail("anchor references air");
        }
        std::set<EditorObjectId> visited;
        auto cursor = object.parent;
        while (cursor) {
            if (!visited.insert(*cursor).second || *cursor == id) return fail("object hierarchy cycle");
            const EditorObject* parent = find_object(*cursor);
            cursor = parent ? parent->parent : std::nullopt;
        }
    }
    return true;
}

EditorSceneSaveResult EditorDocument::save_transactional(const std::filesystem::path& manifestPath) {
    EditorSceneSaveResult result;
    result.manifestPath = manifestPath;
    try {
        refresh_attachment_world_transforms();
        std::string validationError;
        if (!validate(&validationError)) throw std::runtime_error(validationError);
        const std::uint64_t newRevision = revision_ + 1;
        const std::filesystem::path revisionPath = make_revision_path(manifestPath, newRevision);
        const std::filesystem::path stagingPath = revisionPath.string() + ".staging";
        std::error_code ec;
        std::filesystem::remove_all(stagingPath, ec);
        std::filesystem::create_directories(stagingPath);

        for (const auto& [id, object] : objects_) {
            std::string writeError;
            if (object.polygon) {
                auto mesh=*editor_surface_mesh(object);mesh.objectId=id;mesh.contentHash=polygon_asset_content_hash(mesh);
                if (!write_dmesh(stagingPath/(std::to_string(id)+".dmesh"),mesh,&writeError)) throw std::runtime_error(writeError);
                if (object.subdivision && !write_dmesh(stagingPath/(std::to_string(id)+".cage.dmesh"),*object.polygon,&writeError))
                    throw std::runtime_error(writeError);
                continue;
            }
            if (object.text3d) {
                CookedText3DAsset text = *object.text3d;
                text.objectId = id;
                text.sideMesh.objectId = id;
                text.sideMesh.contentHash = polygon_asset_content_hash(text.sideMesh);
                text.contentHash = text3d_content_hash(text);
                const auto output = stagingPath / (std::to_string(id) + ".dtext");
                if (!write_dtext(output, text, &writeError)) throw std::runtime_error(writeError);
                continue;
            }
            if (object.gaborVolume) {
                GaborVolumeAsset volume = *object.gaborVolume;
                volume.recompute_bounds_and_hash();
                const auto output = stagingPath / (std::to_string(id) + ".dgabor");
                if (!write_dgabor(output, volume, &writeError)) throw std::runtime_error(writeError);
                continue;
            }
            CookedVoxelAsset asset(id);
            asset.voxelSizeMeters = object.voxelSizeMeters;
            asset.voxelScale = object.voxelScale;
            MaterialId maximumMaterial = 0;
            for (const auto& entry : object.voxels->bricks()) {
                const Brick& brick = entry.second;
                brick.occupancy().for_each_set([&](std::uint16_t index) {
                    maximumMaterial = std::max(maximumMaterial, brick.material(index));
                });
            }
            for (const auto& [materialId, definition] : object.voxelMaterials) {
                (void)definition;
                maximumMaterial = std::max(maximumMaterial,materialId);
            }
            asset.materials.reserve(static_cast<std::size_t>(maximumMaterial) + 1U);
            for (std::size_t i = 0; i <= maximumMaterial; ++i) asset.materials.push_back(default_material(i));
            for (const auto& [materialId, definition] : object.voxelMaterials) asset.materials[materialId] = definition;
            auto cloned = clone_voxel_object(*object.voxels);
            asset.object = std::move(*cloned);
            const auto output = stagingPath / (std::to_string(id) + ".dvox");
            if (!write_dvox(output, asset, {}, &writeError)) throw std::runtime_error(writeError);
        }
        std::filesystem::remove_all(revisionPath, ec);
        std::filesystem::rename(stagingPath, revisionPath);

        const std::filesystem::path temporary = manifestPath.string() + ".tmp";
        std::filesystem::create_directories(manifestPath.parent_path().empty() ? "." : manifestPath.parent_path());
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) throw std::runtime_error("could not open temporary scene manifest");
        // Format 7 only adds owned polygon objects (geometry kind 3). Scenes without them stay
        // format 6, so editors from before #111 can still open them.
        const bool hasPolygons = std::any_of(objects_.begin(), objects_.end(),
                                             [](const auto& entry) { return entry.second.polygon.has_value(); });
        const bool hasVoxelPalettes = std::any_of(objects_.begin(),objects_.end(),
            [](const auto& entry) { return !entry.second.voxelMaterials.empty(); });
        // Format 8 adds only explicit palette ID lists; material definitions remain in DVOX.
        const bool hasSubdivisions=std::any_of(objects_.begin(),objects_.end(),
            [](const auto& entry) {return entry.second.subdivision.has_value();});
        const bool hasModeling=std::any_of(objects_.begin(),objects_.end(),[](const auto& entry){return entry.second.modelingTopology.has_value();});
        const bool hasRetiredModeling=std::any_of(objects_.begin(),objects_.end(),[](const auto& entry){return !entry.second.modelingTopology&&entry.second.modelingNextId>1;});
        const bool hasSelectionSets = !selectionSets_.empty() || !selectionLineage_.empty();
        output << "DVE_EDITOR_SCENE " << (hasSelectionSets ? 13 : hasRetiredModeling ? 12 : hasModeling ? 11 : hasSubdivisions ? 10 : hasVoxelPalettes ? 8 : hasPolygons ? 7 : 6) << "\n";
        output << "name " << quote_text(name_) << "\n";
        output << "revision " << newRevision << "\n";
        output << "revision_dir " << quote_text(revisionPath.filename().generic_string()) << "\n";
        output << "objects " << objects_.size() << "\n";
        output << std::setprecision(9);
        for (const auto& [id, object] : objects_) {
            output << "object " << id << ' ' << (object.parent ? *object.parent : 0) << ' '
                   << (object.subdivision ? 4 : object.polygon ? 3 : object.text3d ? 1 : object.gaborVolume ? 2 : 0) << ' '
                   << quote_text(object.name) << ' '
                   << quote_text(object.sourceAsset.generic_string()) << ' '
                   << quote_text(object.importRecipe.generic_string()) << ' '
                   << quote_text(object.textFontAsset.generic_string()) << ' '
                   << object.voxelSizeMeters << ' '
                   << object.voxelScale.x << ' ' << object.voxelScale.y << ' ' << object.voxelScale.z << ' '
                   << object.transform.position.x << ' ' << object.transform.position.y << ' ' << object.transform.position.z << ' '
                   << object.transform.rotation.x << ' ' << object.transform.rotation.y << ' '
                   << object.transform.rotation.z << ' ' << object.transform.rotation.w << ' '
                   << object.flags.visible << ' ' << object.flags.locked << ' ' << object.flags.anchored << ' '
                   << object.flags.structural << ' ' << object.flags.collisionEnabled << ' ' << object.flags.decorative << ' '
                   << object.layer << ' ' << object.tags.size();
            for (const std::string& tag : object.tags) output << ' ' << std::quoted(tag);
            output << ' ' << object.groups.size();
            for (const std::string& group : object.groups) output << ' ' << std::quoted(group);
            output << ' ' << object.anchors.size();
            for (const Int3 anchor : object.anchors) output << ' ' << anchor.x << ' ' << anchor.y << ' ' << anchor.z;
            output << "\n";
        }

        std::size_t attachmentCount = 0U;
        std::size_t componentCount = 0U;
        std::size_t prefabLinkCount = 0U;
        for (const auto& [id, object] : objects_) {
            (void)id;
            attachmentCount += object.attachment.has_value() ? 1U : 0U;
            componentCount += object.components.size();
            prefabLinkCount += object.prefabLink.has_value() ? 1U : 0U;
        }
        output << "attachments " << attachmentCount << "\n";
        for (const auto& [id, object] : objects_) {
            if (!object.attachment) continue;
            const EditorAttachment& attachment = *object.attachment;
            output << "attachment " << id << ' ' << attachment.inheritPosition << ' '
                   << attachment.inheritRotation << ' ' << std::quoted(attachment.socket) << ' '
                   << attachment.localTransform.position.x << ' ' << attachment.localTransform.position.y << ' '
                   << attachment.localTransform.position.z << ' ' << attachment.localTransform.rotation.x << ' '
                   << attachment.localTransform.rotation.y << ' ' << attachment.localTransform.rotation.z << ' '
                   << attachment.localTransform.rotation.w << "\n";
        }
        output << "components " << componentCount << "\n";
        for (const auto& [id, object] : objects_) {
            for (const Component& component : object.components) {
                output << "component " << id << ' ' << component.id << ' ' << component.enabled << ' '
                       << std::quoted(component.type) << ' ' << component.properties.size() << "\n";
                for (const auto& [property, value] : component.properties) {
                    output << "property " << std::quoted(property);
                    write_component_value(output, value);
                    output << "\n";
                }
            }
        }
        output << "prefab_links " << prefabLinkCount << "\n";
        for (const auto& [id, object] : objects_) {
            if (!object.prefabLink) continue;
            const EditorPrefabLink& link = *object.prefabLink;
            output << "prefab_link " << id << ' ' << link.instanceId << ' ' << link.templateObjectId << ' '
                   << link.instanceRoot << ' ' << link.sourceContentHash << ' '
                   << std::quoted(link.prefabAsset.generic_string()) << ' ' << link.overrides.size() << "\n";
            for (const auto& [propertyPath, value] : link.overrides) {
                output << "override " << std::quoted(propertyPath);
                write_component_value(output, value);
                output << "\n";
            }
        }
        output << "next_prefab_instance " << nextPrefabInstanceId_ << "\n";
        if (hasVoxelPalettes || hasSubdivisions || hasModeling || hasRetiredModeling || hasSelectionSets) {
            const auto count = std::count_if(objects_.begin(),objects_.end(),
                [](const auto& entry) { return !entry.second.voxelMaterials.empty(); });
            output << "voxel_palettes " << count << "\n";
            for (const auto& [id, object] : objects_) {
                if (object.voxelMaterials.empty()) continue;
                output << "voxel_palette " << id << ' ' << object.voxelMaterials.size();
                for (const auto& [materialId, definition] : object.voxelMaterials) {
                    (void)definition; output << ' ' << unsigned(materialId);
                }
                output << "\n";
            }
        }
        if (hasSubdivisions || hasModeling || hasRetiredModeling || hasSelectionSets) {
            const auto count=std::count_if(objects_.begin(),objects_.end(),
                [](const auto& entry) {return entry.second.subdivision.has_value();});
            output << "subdivisions " << count << "\n";
            for (const auto& [id,object] : objects_) if (object.subdivision) {
                auto settings=*object.subdivision;freeze_subdivision_topology(*object.polygon,settings);
                output << "subdivision " << id << ' ' << settings.level << ' ' << settings.preserveSharpEdges << "\n";
                output << "control_faces " << settings.controlFaces.size() << "\n";
                for (const auto& face : settings.controlFaces) {
                    output << "control_face " << face.size();
                    for (auto index : face) output << ' ' << index;
                    output << "\n";
                }
                output << "edge_creases " << settings.edgeCreases.size() << "\n";
                for (const auto& [edge,strength] : settings.edgeCreases)
                    output << "edge_crease " << edge << ' ' << strength << "\n";
            }
        }
        if(hasModeling || hasRetiredModeling || hasSelectionSets) {
            const auto count=std::count_if(objects_.begin(),objects_.end(),[](const auto& entry){return entry.second.modelingTopology.has_value();});
            output<<"modeling_topologies "<<count<<"\n";
            for(const auto& [id,object]:objects_)if(object.modelingTopology) {
                auto topology=*object.modelingTopology;topology.nextId=(std::max)(topology.nextId,object.modelingNextId);
                output<<"modeling_object "<<id<<"\n";write_modeling_topology(output,topology);
            }
        }
        if(hasRetiredModeling || hasSelectionSets) {
            const auto count=std::count_if(objects_.begin(),objects_.end(),[](const auto& entry){return !entry.second.modelingTopology&&entry.second.modelingNextId>1;});
            output<<"modeling_floors "<<count<<"\n";
            for(const auto& [id,object]:objects_)if(!object.modelingTopology&&object.modelingNextId>1)output<<"modeling_floor "<<id<<' '<<object.modelingNextId<<"\n";
        }
        if (hasSelectionSets) {
            output << "selection_sets " << selectionSets_.size() << "\n";
            for (const EditorSelectionSet& set : selectionSets_) {
                output << "selection_set " << unsigned(set.kind) << ' ' << set.mesh << ' ' << std::quoted(set.name) << ' '
                       << std::quoted(set.query) << ' ' << set.objects.size();
                for (const EditorObjectId id : set.objects) output << ' ' << id;
                output << ' ' << set.elements.size();
                for (const ModelingId id : set.elements) output << ' ' << id;
                output << "\n";
            }
            std::size_t entries = 0;
            for (const auto& [mesh, lineage] : selectionLineage_) entries += lineage.size();
            output << "selection_lineage " << entries << "\n";
            for (const auto& [mesh, lineage] : selectionLineage_)
                for (const auto& [id, next] : lineage) {
                    output << "selection_descendants " << mesh << ' ' << id << ' ' << next.size();
                    for (const ModelingId n : next) output << ' ' << n;
                    output << "\n";
                }
        }
        output.flush();
        if (!output) throw std::runtime_error("failed to write scene manifest");
        output.close();
        std::filesystem::rename(temporary, manifestPath);
        path_ = manifestPath;
        revision_ = newRevision;
        dirty_ = false;
        result.success = true;
        result.revisionDirectory = revisionPath;
    } catch (const std::exception& exception) {
        result.error = exception.what();
    }
    return result;
}

std::optional<EditorDocument> EditorDocument::load(const std::filesystem::path& manifestPath, std::string* error) {
    try {
        std::ifstream input(manifestPath, std::ios::binary);
        if (!input) throw std::runtime_error("could not open editor scene");
        std::string magic;
        std::uint32_t version{};
        input >> magic >> version;
        if (magic != "DVE_EDITOR_SCENE" || (version != 1 && version != 2 && version != 3 && version != 4 && version != 5 && version != 6 && version != 7 && version != 8 && version != 9 && version != 10 && version != 11 && version != 12 && version != 13)) throw std::runtime_error("unsupported editor scene format");
        std::string key;
        std::string name;
        std::uint64_t revision{};
        std::string revisionDirectory;
        std::size_t objectCount{};
        input >> key >> std::quoted(name); if (key != "name") throw std::runtime_error("missing scene name");
        input >> key >> revision; if (key != "revision") throw std::runtime_error("missing revision");
        input >> key >> std::quoted(revisionDirectory); if (key != "revision_dir") throw std::runtime_error("missing revision directory");
        input >> key >> objectCount; if (key != "objects") throw std::runtime_error("missing object count");
        EditorDocument document(name);
        std::map<EditorObjectId,std::vector<VoxelMaterialDefinition>> loadedPalettes;
        for (std::size_t i = 0; i < objectCount; ++i) {
            EditorObjectId id{}, parent{};
            std::uint32_t geometryKind{};
            std::string objectName, source, recipe, fontSource;
            EditorObject object;
            std::size_t anchorCount{};
            input >> key >> id >> parent;
            if (version >= 2) input >> geometryKind;
            input >> std::quoted(objectName) >> std::quoted(source) >> std::quoted(recipe);
            if (version >= 2) input >> std::quoted(fontSource);
            input >> object.voxelSizeMeters;
            if (version >= 6) input >> object.voxelScale.x >> object.voxelScale.y >> object.voxelScale.z;
            input >> object.transform.position.x >> object.transform.position.y >> object.transform.position.z
                  >> object.transform.rotation.x >> object.transform.rotation.y >> object.transform.rotation.z >> object.transform.rotation.w
                  >> object.flags.visible >> object.flags.locked >> object.flags.anchored >> object.flags.structural
                  >> object.flags.collisionEnabled >> object.flags.decorative;
            if (version >= 5) {
                std::size_t tagCount{}, groupCount{};
                input >> object.layer >> tagCount;
                if (tagCount > 1024U) throw std::runtime_error("too many object tags");
                object.tags.resize(tagCount);
                for (std::string& tag : object.tags) input >> std::quoted(tag);
                input >> groupCount;
                if (groupCount > 1024U) throw std::runtime_error("too many object groups");
                object.groups.resize(groupCount);
                for (std::string& group : object.groups) input >> std::quoted(group);
            }
            input >> anchorCount;
            if (!input || key != "object" || geometryKind > (version>=9?4U:version>=7?3U:2U)) throw std::runtime_error("malformed object record");
            object.id = id;
            object.name = objectName;
            object.parent = parent == 0 ? std::nullopt : std::optional<EditorObjectId>(parent);
            object.sourceAsset = source;
            object.importRecipe = recipe;
            object.textFontAsset = fontSource;
            if (geometryKind == 3U || geometryKind == 4U) {
                auto loaded=read_dmesh(manifestPath.parent_path()/revisionDirectory/
                    (std::to_string(id)+(geometryKind==4U?".cage.dmesh":".dmesh")));
                if (geometryKind==4U) object.subdivision=SubdivisionSettings{};
                if (!loaded) throw std::runtime_error("could not load polygon object: "+loaded.error);
                object.polygon=std::move(loaded.asset);object.voxels=std::make_unique<VoxelObject>(id);
            } else if (geometryKind == 1U) {
                const auto dtext = manifestPath.parent_path() / revisionDirectory / (std::to_string(id) + ".dtext");
                Text3DReadResult loaded = read_dtext(dtext);
                if (!loaded) throw std::runtime_error("could not load 3D text object " + std::to_string(id) + ": " + loaded.error);
                object.text3d = std::move(loaded.asset);
                object.voxels = std::make_unique<VoxelObject>(id);
            } else if (geometryKind == 2U) {
                const auto dgabor = manifestPath.parent_path() / revisionDirectory / (std::to_string(id) + ".dgabor");
                GaborImportResult loaded = read_dgabor(dgabor);
                if (!loaded) throw std::runtime_error("could not load Gabor volume object " + std::to_string(id) + ": " + loaded.error);
                object.gaborVolume = std::move(loaded.asset);
                object.voxels = std::make_unique<VoxelObject>(id);
            } else {
                const auto dvox = manifestPath.parent_path() / revisionDirectory / (std::to_string(id) + ".dvox");
                DvoxReadResult loaded = read_dvox(dvox);
                if (!loaded.success) throw std::runtime_error("could not load object " + std::to_string(id) + ": " + loaded.error);
                object.voxels = std::make_unique<VoxelObject>(std::move(loaded.asset.object));
                if (version >= 8) loadedPalettes.emplace(id,std::move(loaded.asset.materials));
            }
            for (std::size_t a = 0; a < anchorCount; ++a) {
                Int3 anchor{}; input >> anchor.x >> anchor.y >> anchor.z; object.anchors.insert(anchor);
            }
            document.add_object(std::move(object));
        }
        if (version >= 4) {
            std::size_t attachmentCount{};
            input >> key >> attachmentCount;
            if (!input || key != "attachments") throw std::runtime_error("missing attachment section");
            for (std::size_t i = 0; i < attachmentCount; ++i) {
                EditorObjectId objectId{};
                EditorAttachment attachment;
                input >> key >> objectId >> attachment.inheritPosition >> attachment.inheritRotation
                      >> std::quoted(attachment.socket)
                      >> attachment.localTransform.position.x >> attachment.localTransform.position.y
                      >> attachment.localTransform.position.z >> attachment.localTransform.rotation.x
                      >> attachment.localTransform.rotation.y >> attachment.localTransform.rotation.z
                      >> attachment.localTransform.rotation.w;
                EditorObject* object = document.find_object(objectId);
                if (!input || key != "attachment" || !object) throw std::runtime_error("malformed attachment record");
                object->attachment = std::move(attachment);
            }
            std::size_t componentCount{};
            input >> key >> componentCount;
            if (!input || key != "components") throw std::runtime_error("missing component section");
            for (std::size_t i = 0; i < componentCount; ++i) {
                EditorObjectId objectId{};
                Component component;
                std::size_t propertyCount{};
                input >> key >> objectId >> component.id >> component.enabled >> std::quoted(component.type) >> propertyCount;
                EditorObject* object = document.find_object(objectId);
                if (!input || key != "component" || !object) throw std::runtime_error("malformed component record");
                for (std::size_t p = 0; p < propertyCount; ++p) {
                    std::string property;
                    input >> key >> std::quoted(property);
                    if (!input || key != "property") throw std::runtime_error("malformed component property");
                    component.properties.emplace(std::move(property), read_component_value(input));
                }
                object->components.push_back(std::move(component));
            }
            for (auto& [objectId, object] : document.objects_) {
                (void)objectId;
                if (const Component* membership = find_component_by_type(
                        std::span<const Component>(object.components), "dve.membership")) {
                    if (const auto it = membership->properties.find("tags"); it != membership->properties.end())
                        if (const auto* value = std::get_if<std::string>(&it->second)) object.tags = parse_membership_values(*value);
                    if (const auto it = membership->properties.find("groups"); it != membership->properties.end())
                        if (const auto* value = std::get_if<std::string>(&it->second)) object.groups = parse_membership_values(*value);
                    if (const auto it = membership->properties.find("layer"); it != membership->properties.end())
                        if (const auto* value = std::get_if<std::int64_t>(&it->second))
                            object.layer = *value >= 0 && *value <= 63 ? static_cast<std::uint32_t>(*value) : 0U;
                }
            }
            std::size_t prefabLinkCount{};
            input >> key >> prefabLinkCount;
            if (!input || key != "prefab_links") throw std::runtime_error("missing prefab-link section");
            for (std::size_t i = 0; i < prefabLinkCount; ++i) {
                EditorObjectId objectId{};
                EditorPrefabLink link;
                std::string prefabAsset;
                std::size_t overrideCount{};
                input >> key >> objectId >> link.instanceId >> link.templateObjectId >> link.instanceRoot;
                if (version >= 5) input >> link.sourceContentHash;
                input >> std::quoted(prefabAsset) >> overrideCount;
                EditorObject* object = document.find_object(objectId);
                if (!input || key != "prefab_link" || !object) throw std::runtime_error("malformed prefab-link record");
                link.prefabAsset = prefabAsset;
                if (version < 5) link.sourceContentHash = 1U;
                for (std::size_t o = 0; o < overrideCount; ++o) {
                    std::string propertyPath;
                    input >> key >> std::quoted(propertyPath);
                    if (!input || key != "override") throw std::runtime_error("malformed prefab override");
                    link.overrides.emplace(std::move(propertyPath), read_component_value(input));
                }
                object->prefabLink = std::move(link);
            }
            input >> key >> document.nextPrefabInstanceId_;
            if (!input || key != "next_prefab_instance") throw std::runtime_error("missing prefab-instance counter");
            document.refresh_attachment_world_transforms();
        }
        if (version >= 8) {
            std::size_t paletteCount{};
            input >> key >> paletteCount;
            if (!input || key != "voxel_palettes" || paletteCount > objectCount)
                throw std::runtime_error("invalid voxel palette section");
            std::set<EditorObjectId> paletteObjects;
            for (std::size_t i = 0; i < paletteCount; ++i) {
                EditorObjectId id{}; std::size_t count{};
                input >> key >> id >> count;
                auto* object = document.find_object(id);
                const auto palette = loadedPalettes.find(id);
                if (!input || key != "voxel_palette" || !object || object->polygon || object->text3d ||
                    object->gaborVolume || count > 255 || palette == loadedPalettes.end() ||
                    !paletteObjects.insert(id).second) throw std::runtime_error("invalid voxel palette record");
                for (std::size_t j = 0; j < count; ++j) {
                    unsigned materialId{}; input >> materialId;
                    if (!input || materialId == 0 || materialId >= palette->second.size() || materialId > 255 ||
                        !object->voxelMaterials.emplace(static_cast<MaterialId>(materialId),palette->second[materialId]).second)
                        throw std::runtime_error("invalid voxel palette material");
                }
            }
        }
        if (version>=9) {
            std::size_t count{};input >> key >> count;
            const auto expected=std::count_if(document.objects_.begin(),document.objects_.end(),
                [](const auto& entry) {return entry.second.subdivision.has_value();});
            if (!input || key!="subdivisions" || count!=static_cast<std::size_t>(expected))
                throw std::runtime_error("invalid subdivision section");
            std::set<EditorObjectId> seen;
            for (std::size_t i=0;i<count;++i) {
                EditorObjectId id{};SubdivisionSettings settings;unsigned sharp{};
                input >> key >> id >> settings.level >> sharp;
                auto* object=document.find_object(id);
                if (!input || key!="subdivision" || !object || !object->subdivision || settings.level>kMaxSubdivisionLevel ||
                    sharp>1 || !seen.insert(id).second) throw std::runtime_error("invalid subdivision record");
                settings.preserveSharpEdges=sharp!=0;
                if (version>=10) {
                    std::size_t faceCount{},cornersTotal{};input >> key >> faceCount;
                    if (!input || key!="control_faces" || faceCount==0 || faceCount>kMaxSubdivisionControlTriangles)
                        throw std::runtime_error("invalid control face count");
                    for (std::size_t f=0;f<faceCount;++f) {
                        std::size_t corners{};input >> key >> corners;
                        if (!input || key!="control_face" || (corners!=3 && corners!=6) ||
                            (cornersTotal+=corners)>kMaxSubdivisionControlTriangles*3)
                            throw std::runtime_error("invalid control face record");
                        std::vector<std::uint32_t> face(corners);
                        for (auto& index : face) {
                            input >> index;
                            if (!input || index>=object->polygon->vertices.size()) throw std::runtime_error("invalid control face vertex");
                        }
                        settings.controlFaces.push_back(std::move(face));
                    }
                    std::size_t creaseCount{};input >> key >> creaseCount;
                    if (!input || key!="edge_creases" || creaseCount>kMaxSubdivisionControlTriangles*3)
                        throw std::runtime_error("invalid edge crease count");
                    for (std::size_t e=0;e<creaseCount;++e) {
                        std::uint64_t edge{};float strength{};input >> key >> edge >> strength;
                        if (!input || key!="edge_crease" || !std::isfinite(strength) || strength<=0 || strength>4 ||
                            !settings.edgeCreases.emplace(edge,strength).second)
                            throw std::runtime_error("invalid edge crease record");
                    }
                } else freeze_subdivision_topology(*object->polygon,settings);
                object->subdivision=std::move(settings);
            }
        }
        if(version>=11) {
            std::size_t count{};input>>key>>count;
            if(!input||key!="modeling_topologies"||count>document.objects_.size())throw std::runtime_error("Invalid modeling topology count");
            std::set<EditorObjectId> seen;
            for(std::size_t i=0;i<count;++i) {
                EditorObjectId id{};input>>key>>id;auto* object=document.find_object(id);
                if(!input||key!="modeling_object"||!object||!object->polygon||!seen.insert(id).second)throw std::runtime_error("Invalid modeling object record");
                std::string reason;auto topology=read_modeling_topology(input,&reason);
                if(!topology||!validate_modeling_topology(*object->polygon,object->subdivision?&*object->subdivision:nullptr,*topology,&reason))throw std::runtime_error(reason);
                object->modelingNextId=topology->nextId;object->modelingTopology=std::move(topology);
            }
        }
        if(version>=12) {
            std::size_t count{};input>>key>>count;if(!input||key!="modeling_floors"||count>document.objects_.size())throw std::runtime_error("Invalid retired modeling namespace count");
            std::set<EditorObjectId> seen;
            for(std::size_t i=0;i<count;++i) {
                EditorObjectId id{};ModelingId floor{};input>>key>>id>>floor;auto* object=document.find_object(id);
                if(!input||key!="modeling_floor"||!object||object->modelingTopology||floor<=1||!seen.insert(id).second)throw std::runtime_error("Invalid retired modeling namespace record");
                object->modelingNextId=floor;
            }
        }
        if (version >= 13) {
            // Named selection sets. Members of deleted objects are kept (they show as missing).
            constexpr std::size_t kMaxRecords = 1U << 22U;
            std::size_t count{};
            input >> key >> count;
            if (!input || key != "selection_sets" || count > 65536U) throw std::runtime_error("invalid selection set section");
            for (std::size_t i = 0; i < count; ++i) {
                EditorSelectionSet set;
                unsigned kind{};
                std::size_t objectCount{}, elementCount{};
                input >> key >> kind >> set.mesh >> std::quoted(set.name) >> std::quoted(set.query) >> objectCount;
                if (!input || key != "selection_set" || kind > unsigned(SelectionSetKind::Query) || objectCount > kMaxRecords)
                    throw std::runtime_error("invalid selection set record");
                set.kind = static_cast<SelectionSetKind>(kind);
                for (std::size_t j = 0; j < objectCount; ++j) { EditorObjectId id{}; input >> id; set.objects.insert(id); }
                input >> elementCount;
                if (!input || elementCount > kMaxRecords) throw std::runtime_error("invalid selection set members");
                for (std::size_t j = 0; j < elementCount; ++j) { ModelingId id{}; input >> id; set.elements.insert(id); }
                if (!input) throw std::runtime_error("invalid selection set members");
                document.selectionSets_.push_back(std::move(set));
            }
            input >> key >> count;
            if (!input || key != "selection_lineage" || count > kMaxRecords) throw std::runtime_error("invalid selection lineage section");
            for (std::size_t i = 0; i < count; ++i) {
                EditorObjectId mesh{};
                ModelingId id{};
                std::size_t next{};
                input >> key >> mesh >> id >> next;
                if (!input || key != "selection_descendants" || next > kMaxRecords) throw std::runtime_error("invalid selection lineage record");
                auto& descendants = document.selectionLineage_[mesh][id];
                for (std::size_t j = 0; j < next; ++j) { ModelingId n{}; input >> n; descendants.insert(n); }
                if (!input) throw std::runtime_error("invalid selection lineage record");
            }
        }
        document.path_ = manifestPath;
        document.revision_ = revision;
        document.dirty_ = false;
        std::string validationError;
        if (!document.validate(&validationError)) throw std::runtime_error(validationError);
        return document;
    } catch (const std::exception& exception) {
        if (error) *error = exception.what();
        return std::nullopt;
    }
}

} // namespace dve::editor
