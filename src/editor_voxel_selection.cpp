#include "dve/editor_voxel_selection.hpp"
#include "dve/voxel_metric.hpp"
#include "dve/editor_polygon.hpp"
#include "dve/editor_subdivision.hpp"

#include <algorithm>
#include <map>
#include <array>
#include <cmath>
#include <limits>

#include "dve/editor_voxel_boolean.hpp"

namespace dve::editor {
namespace {

constexpr Int3 kFaces[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};

Int3 add(Int3 a, Int3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }

template <typename Fn>
void for_each_occupied(const VoxelObject& voxels, Fn&& fn) {
    for (const auto& [key, brick] : voxels.bricks())
        brick.occupancy().for_each_set([&](std::uint16_t index) {
            fn(global_from_local(key, local_from_index_unchecked(index)), brick.material(index));
        });
}


bool sample_hits_cell(const EditorDocument& document, EditorObjectId object, Int3 cell,
                     const EditorCamera& camera, UiRect viewport, float x, float y,
                     const std::set<EditorObjectId>* onlyObjects, std::size_t& rayBudget) {
    if (rayBudget == 0) return false;
    --rayBudget;
    auto ray = make_viewport_ray(camera,viewport,x,y);
    const float cosine = dot(ray.direction,camera_basis(camera).forward);
    if (cosine <= 0 || camera.farPlane <= camera.nearPlane) return false;
    const float nearDistance = std::max(0.00001F,camera.nearPlane);
    ray.origin = add(ray.origin,multiply(ray.direction,nearDistance/cosine));
    const auto hit = pick_editor_document(document,ray,(camera.farPlane-nearDistance)/cosine,onlyObjects);
    return hit && hit->objectId == object && !hit->polygonFace && hit->voxel == cell;
}

std::string object_problem(const EditorObject& object) {
    if (!object.voxels || object.polygon || object.text3d || object.gaborVolume) return "Voxel selection works on voxel objects";
    if (object.flags.locked) return "Unlock " + object.name + " first";
    return {};
}

} // namespace

std::set<Int3> voxel_connected_piece(const VoxelObject& voxels, Int3 seed, std::size_t limit) {
    std::set<Int3> piece;
    if (!voxels.occupied_at(seed)) return piece;
    std::vector<Int3> stack{seed};
    piece.insert(seed);
    while (!stack.empty() && piece.size() <= limit) {
        const Int3 at = stack.back();
        stack.pop_back();
        for (const Int3 face : kFaces) {
            const Int3 next = add(at, face);
            if (piece.contains(next) || !voxels.occupied_at(next)) continue;
            piece.insert(next);
            if (piece.size() > limit) break;
            stack.push_back(next);
        }
    }
    return piece;
}

std::set<Int3> all_occupied_voxels(const VoxelObject& voxels) {
    std::set<Int3> cells;
    for_each_occupied(voxels, [&](Int3 cell, MaterialId) { cells.insert(cell); });
    return cells;
}

std::set<Int3> grow_voxel_selection(const VoxelObject& voxels, const std::set<Int3>& selection, std::size_t limit) {
    std::set<Int3> grown = selection;
    for (const Int3 cell : selection) {
        if (grown.size() > limit) break;
        for (const Int3 face : kFaces) {
            const Int3 next = add(cell, face);
            if (voxels.occupied_at(next)) grown.insert(next);
        }
    }
    return grown;
}

std::set<Int3> shrink_voxel_selection(const std::set<Int3>& selection) {
    std::set<Int3> shrunk;
    for (const Int3 cell : selection) {
        bool interior = true;
        for (const Int3 face : kFaces) interior = interior && selection.contains(add(cell, face));
        if (interior) shrunk.insert(cell);
    }
    return shrunk;
}

std::set<Int3> invert_voxel_selection(const VoxelObject& voxels, const std::set<Int3>& selection, std::size_t limit) {
    std::set<Int3> inverted;
    for_each_occupied(voxels, [&](Int3 cell, MaterialId) {
        if (inverted.size() <= limit && !selection.contains(cell)) inverted.insert(cell);
    });
    return inverted;
}

std::set<Int3> voxels_in_screen_rect(const EditorObject& object, const EditorCamera& camera, UiRect viewport,
                                     UiRect rect, bool through, bool crossing, const EditorDocument* document,
                                     const std::set<EditorObjectId>* onlyObjects, std::string* error) {
    std::set<Int3> cells;
    if (!object.voxels || object.polygon || object.text3d || object.gaborVolume ||
        viewport.width <= 0 || viewport.height <= 0 || rect.width <= 0 || rect.height <= 0) return cells;
    if (error) error->clear();
    std::size_t rayBudget = kMaxVoxelSelectionRays;
    bool exhausted = false;
    const auto basis = camera_basis(camera);
    const float aspect = float(viewport.width)/float(viewport.height);
    const float tangent = std::tan(std::clamp(editor_camera_vertical_fov(camera,aspect),0.1F,3.04159265F)*0.5F);
    const float halfHeight = std::max(0.001F,camera.orthographicHeight*0.5F);
    const float nearDepth = std::max(0.00001F,camera.nearPlane);
    const float farDepth = camera.farPlane;
    if (!(farDepth > nearDepth)) return cells;
    struct Point { float x{},y{}; };
    const float left = float(std::max(rect.x,viewport.x)), top = float(std::max(rect.y,viewport.y));
    const float right = float(std::min(rect.x+rect.width,viewport.x+viewport.width));
    const float bottom = float(std::min(rect.y+rect.height,viewport.y+viewport.height));
    if (right < left || bottom < top) return cells;
    const auto inside = [&](Point p) { return p.x >= left && p.x <= right && p.y >= top && p.y <= bottom; };
    const auto project = [&](Float3 p) {
        const bool ortho = camera.projection == EditorProjection::Orthographic;
        const float nx = ortho ? p.x/(halfHeight*aspect) :
            p.x/(p.z*tangent*aspect) + (camera.physicalLens.enabled ? camera.physicalLens.lensShiftX*2.0F : 0.0F);
        const float ny = ortho ? p.y/halfHeight :
            p.y/(p.z*tangent) + (camera.physicalLens.enabled ? camera.physicalLens.lensShiftY*2.0F : 0.0F);
        return Point{float(viewport.x)+(nx+1.0F)*0.5F*float(viewport.width),
                     float(viewport.y)+(1.0F-ny)*0.5F*float(viewport.height)};
    };
    const auto turn = [](Point a,Point b,Point c) { return (b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x); };
    for_each_occupied(*object.voxels, [&](Int3 cell, MaterialId) {
        if (exhausted || cells.size() > kMaxVoxelSelection) return; // Caller refuses an over-budget box.
        const Float3 centre = voxel_to_world(object.transform,multiply(object.voxelScale,object.voxelSizeMeters),
            {float(cell.x)+0.5F,float(cell.y)+0.5F,float(cell.z)+0.5F});
        if (!through) {
            const Float3 toCamera = camera.projection == EditorProjection::Orthographic
                ? multiply(basis.forward,-1.0F) : subtract(camera.position,centre);
            bool facing = false;
            for (const auto face : kFaces) {
                const std::int64_t x = std::int64_t(cell.x)+face.x, y = std::int64_t(cell.y)+face.y, z = std::int64_t(cell.z)+face.z;
                const bool valid = x >= kInt3Min.x && x <= kInt3Max.x && y >= kInt3Min.y && y <= kInt3Max.y &&
                                   z >= kInt3Min.z && z <= kInt3Max.z;
                if (valid && object.voxels->occupied_at({static_cast<std::int32_t>(x),static_cast<std::int32_t>(y),
                                                       static_cast<std::int32_t>(z)})) continue;
                const Float3 normal = rotate(object.transform.rotation,{float(face.x),float(face.y),float(face.z)});
                if (dot(normal,toCamera) > 0.0F) { facing = true; break; }
            }
            if (!facing) return;
        }
        std::array<Float3,8> corners{};
        std::vector<Point> points;
        bool contained = true;
        for (int i = 0; i < 8; ++i) {
            const auto world = voxel_to_world(object.transform,multiply(object.voxelScale,object.voxelSizeMeters),
                {float(double(cell.x)+((i&1)?1.0:0.0)),float(double(cell.y)+((i&2)?1.0:0.0)),
                 float(double(cell.z)+((i&4)?1.0:0.0))});
            const auto relative = subtract(world,camera.position);
            const Float3 view{dot(relative,basis.right),dot(relative,basis.up),dot(relative,basis.forward)};
            corners[i] = view;
            if (view.z < nearDepth || view.z > farDepth) { contained = false; continue; }
            const auto screen = project(view);
            if (!std::isfinite(screen.x) || !std::isfinite(screen.y)) { contained = false; continue; }
            contained = contained && inside(screen);
            points.push_back(screen);
        }
        if (!crossing && !contained) return;
        // Near/far clipping creates cap vertices. Keep them even when original corners are
        // off screen; crossing operates on the actual projected convex cell, not its centre.
        for (int i = 0; i < 8; ++i)
            for (int bit : {1,2,4}) {
                const int j = i^bit;
                if (i >= j) continue;
                for (float depth : {nearDepth,farDepth}) {
                    const auto a = corners[i], b = corners[j];
                    if ((a.z < depth && b.z > depth) || (b.z < depth && a.z > depth)) {
                        const float t = (depth-a.z)/(b.z-a.z);
                        const auto screen = project({a.x+(b.x-a.x)*t,a.y+(b.y-a.y)*t,depth});
                        if (std::isfinite(screen.x) && std::isfinite(screen.y)) points.push_back(screen);
                    }
                }
            }
        if (points.empty()) return;
        std::sort(points.begin(),points.end(),[](Point a,Point b) { return a.x < b.x || (a.x == b.x && a.y < b.y); });
        std::vector<Point> hull;
        for (const auto point : points) {
            while (hull.size() >= 2 && turn(hull[hull.size()-2],hull.back(),point) <= 0) hull.pop_back();
            hull.push_back(point);
        }
        const auto lower = hull.size();
        for (std::size_t i = points.size()-1; i > 0; --i) {
            const auto point = points[i-1];
            while (hull.size() > lower && turn(hull[hull.size()-2],hull.back(),point) <= 0) hull.pop_back();
            hull.push_back(point);
        }
        if (hull.size() > 1) hull.pop_back();
        // Clip the projected convex hull to the rectangle, avoiding bounding-box false hits.
        for (int plane = 0; plane < 4 && !hull.empty(); ++plane) {
            const auto distance = [&](Point p) {
                return plane == 0 ? p.x-left : plane == 1 ? right-p.x : plane == 2 ? p.y-top : bottom-p.y;
            };
            std::vector<Point> clipped;
            auto previous = hull.back();
            float before = distance(previous);
            for (const auto point : hull) {
                const float after = distance(point);
                if ((before < 0) != (after < 0)) {
                    const float t = before/(before-after);
                    clipped.push_back({previous.x+(point.x-previous.x)*t,previous.y+(point.y-previous.y)*t});
                }
                if (after >= 0) clipped.push_back(point);
                previous = point; before = after;
            }
            hull = std::move(clipped);
        }
        if (hull.empty()) return;
        if (!through && document) {
            Point centre{};
            for (auto p : hull) { centre.x += p.x; centre.y += p.y; }
            centre.x /= float(hull.size()); centre.y /= float(hull.size());
            const auto visible = [&](Point p) {
                if (rayBudget == 0) { exhausted = true; return false; }
                return sample_hits_cell(*document,object.id,cell,camera,viewport,p.x,p.y,onlyObjects,rayBudget);
            };
            bool hit = visible(centre);
            for (std::size_t i = 0; i < hull.size() && !hit && !exhausted; ++i) {
                // Interior-biased corners and edge centres avoid ambiguous shared cell edges.
                const auto a = hull[i], b = hull[(i+1)%hull.size()];
                hit = visible({centre.x*0.1F+a.x*0.9F,centre.y*0.1F+a.y*0.9F}) ||
                      visible({centre.x*0.1F+(a.x+b.x)*0.45F,centre.y*0.1F+(a.y+b.y)*0.45F});
            }
            if (!hit) return;
        }
        cells.insert(cell);
    });
    if (exhausted) {
        cells.clear();
        if (error) *error = "Visible selection exceeds 100000 scene rays; use a smaller box or X-Ray";
    }
    return cells;
}

bool voxel_cell_visible(const EditorDocument& document, const EditorObject& object, Int3 cell,
                        const EditorCamera& camera, UiRect viewport,
                        const std::set<EditorObjectId>* onlyObjects, std::size_t& rayBudget) {
    if (!object.voxels || !object.voxels->occupied_at(cell)) return false;
    for (unsigned axis = 0; axis < 3; ++axis)
        for (int sign : {-1,1}) {
            std::array<std::int64_t,3> neighbour{cell.x,cell.y,cell.z};
            neighbour[axis] += sign;
            const bool valid = neighbour[axis] >= kInt3Min.x && neighbour[axis] <= kInt3Max.x;
            if (valid && object.voxels->occupied_at({static_cast<std::int32_t>(neighbour[0]),
                static_cast<std::int32_t>(neighbour[1]),static_cast<std::int32_t>(neighbour[2])})) continue;
            for (unsigned sample = 0; sample < 5; ++sample) {
                Float3 local{float(cell.x)+0.5F,float(cell.y)+0.5F,float(cell.z)+0.5F};
                float* coordinates[] = {&local.x,&local.y,&local.z};
                *coordinates[axis] += float(sign)*0.5F;
                if (sample != 0) {
                    *coordinates[(axis+1)%3] += (sample&1U) ? 0.35F : -0.35F;
                    *coordinates[(axis+2)%3] += (sample&2U) ? 0.35F : -0.35F;
                }
                const auto world = voxel_to_world(object.transform,multiply(object.voxelScale,object.voxelSizeMeters),local);
                const auto screen = project_world_to_screen(camera,viewport,world);
                if (screen.depth <= camera.nearPlane || screen.depth > camera.farPlane ||
                    !std::isfinite(screen.x) || !std::isfinite(screen.y) ||
                    screen.x < viewport.x || screen.x >= viewport.x+viewport.width ||
                    screen.y < viewport.y || screen.y >= viewport.y+viewport.height) continue;
                if (sample_hits_cell(document,object.id,cell,camera,viewport,screen.x,screen.y,onlyObjects,rayBudget))
                    return true;
                if (rayBudget == 0) return false;
            }
        }
    return false;
}

std::set<Int3> voxels_in_cube(const VoxelObject& voxels, Int3 center, int radius) {
    std::set<Int3> cells;
    const int r = std::clamp(radius, 0, 32);
    for (int x = -r; x <= r; ++x)
        for (int y = -r; y <= r; ++y)
            for (int z = -r; z <= r; ++z) {
                const std::int64_t gx = std::int64_t(center.x)+x, gy = std::int64_t(center.y)+y, gz = std::int64_t(center.z)+z;
                if (gx < kInt3Min.x || gx > kInt3Max.x || gy < kInt3Min.y || gy > kInt3Max.y ||
                    gz < kInt3Min.z || gz > kInt3Max.z) continue;
                const Int3 cell{static_cast<std::int32_t>(gx),static_cast<std::int32_t>(gy),static_cast<std::int32_t>(gz)};
                if (voxels.occupied_at(cell)) cells.insert(cell);
            }
    return cells;
}

std::unique_ptr<IEditorCommand> build_delete_voxels_command(const EditorObject& object, const std::set<Int3>& cells,
                                                            std::string* error) {
    const auto fail = [&](std::string message) -> std::unique_ptr<IEditorCommand> {
        if (error) *error = std::move(message);
        return nullptr;
    };
    if (std::string problem = object_problem(object); !problem.empty()) return fail(problem);
    std::vector<VoxelBooleanChange> changes;
    for (const Int3 cell : cells) {
        const MaterialId material = object.voxels->material_at(cell);
        if (material != kAirMaterial) changes.push_back({cell, material, kAirMaterial});
    }
    if (changes.empty()) return fail("No voxels selected");
    const std::vector<Int3> before(object.anchors.begin(), object.anchors.end());
    std::vector<Int3> after;
    for (const Int3 anchor : object.anchors) if (!cells.contains(anchor)) after.push_back(anchor);
    return std::make_unique<ApplyVoxelBooleanCommand>(object.id, std::move(changes), before, std::move(after),
                                                      "Delete voxels");
}

std::unique_ptr<IEditorCommand> build_move_voxels_command(const EditorObject& object, const std::set<Int3>& cells,
                                                          Int3 offset, std::string* error) {
    const auto fail = [&](std::string message) -> std::unique_ptr<IEditorCommand> {
        if (error) *error = std::move(message);
        return nullptr;
    };
    if (std::string problem = object_problem(object); !problem.empty()) return fail(problem);
    if (offset.x == 0 && offset.y == 0 && offset.z == 0) return fail("The selection did not move");
    // Final state of every cell touched: sources empty first, then destinations take the moved
    // material (overwriting what was there).
    std::map<Int3, MaterialId> final;
    std::map<Int3, MaterialId> moved;
    for (const Int3 cell : cells) {
        const MaterialId material = object.voxels->material_at(cell);
        if (material == kAirMaterial) continue;
        final[cell] = kAirMaterial;
        moved[add(cell, offset)] = material;
    }
    if (moved.empty()) return fail("No voxels selected");
    for (const auto& [cell, material] : moved) final[cell] = material;
    std::vector<VoxelBooleanChange> changes;
    for (const auto& [cell, after] : final) {
        const MaterialId before = object.voxels->material_at(cell);
        if (before != after) changes.push_back({cell, before, after});
    }
    const std::vector<Int3> anchorsBefore(object.anchors.begin(), object.anchors.end());
    std::set<Int3> anchorsAfter;
    for (const Int3 anchor : object.anchors) {
        if (cells.contains(anchor)) anchorsAfter.insert(add(anchor, offset));  // moves with its voxel
        else if (!moved.contains(anchor)) anchorsAfter.insert(anchor);       // untouched; overwritten ones go
    }
    return std::make_unique<ApplyVoxelBooleanCommand>(object.id, std::move(changes), anchorsBefore,
                                                      std::vector<Int3>(anchorsAfter.begin(), anchorsAfter.end()),
                                                      "Move voxels");
}


VoxelRegionBuild build_voxel_region_command(const EditorObject& object, const std::set<Int3>& cells,
                                           VoxelRegionEdit edit, VoxelRegionSettings settings) {
    VoxelRegionBuild result;
    const auto fail = [&](std::string message) {
        result.command.reset();
        result.selection.clear();
        result.error = std::move(message);
        return std::move(result);
    };
    if (auto problem = object_problem(object); !problem.empty()) return fail(problem);
    if (cells.empty()) return fail("Select voxels first");
    if (cells.size() > kMaxVoxelSelection) return fail("Selected region exceeds the voxel edit budget");
    if (edit != VoxelRegionEdit::Extrude && edit != VoxelRegionEdit::RotateQuarterTurn && edit != VoxelRegionEdit::ScaleInteger)
        return fail("Unknown voxel region operation");
    if (settings.axis < 0 || settings.axis > 2) return fail("Choose an object-grid axis X, Y or Z");
    if ((edit == VoxelRegionEdit::Extrude && (settings.amount == 0 || settings.amount < -32 || settings.amount > 32)) ||
        (edit == VoxelRegionEdit::RotateQuarterTurn && settings.amount != 1 && settings.amount != -1) ||
        (edit == VoxelRegionEdit::ScaleInteger && (settings.amount < 2 || settings.amount > 8)))
        return fail("Use 1..32 extrusion layers, a +/-90 degree turn, or integer scale 2..8");
    const auto coords = [](Int3 p) { return std::array<std::int64_t,3>{p.x,p.y,p.z}; };
    const auto checked = [](const std::array<std::int64_t,3>& p, Int3& cell) {
        // Leave a brick margin: derived extents add kBrickDim to the brick origin,
        // and neighbouring-cell selection must remain representable too.
        for (auto value : p) {
            if (value < std::int64_t(std::numeric_limits<std::int32_t>::min()) + kBrickDim ||
                value > std::int64_t(std::numeric_limits<std::int32_t>::max()) - kBrickDim) return false;
        }
        cell = {static_cast<std::int32_t>(p[0]),static_cast<std::int32_t>(p[1]),static_cast<std::int32_t>(p[2])};
        return true;
    };
    Int3 minimum = *cells.begin(), maximum = minimum;
    for (const auto cell : cells) {
        Int3 checkedCell{};
        if (!checked(coords(cell),checkedCell)) return fail("Selected region lies outside the supported integer grid");
        if (!object.voxels->occupied_at(cell)) return fail("Selection changed; select occupied voxels again");
        minimum = min_components(minimum,cell);
        maximum = max_components(maximum,cell);
    }
    const auto lo = coords(minimum), hi = coords(maximum);
    std::array<std::int64_t,3> pivot{};
    for (int axis = 0; axis < 3; ++axis) pivot[axis] = lo[axis] + (hi[axis]-lo[axis])/2;
    if (settings.rotationPivot) {
        Int3 checkedPivot{};
        if (!checked(coords(*settings.rotationPivot),checkedPivot)) return fail("Rotation pivot lies outside the supported integer grid");
        pivot = coords(*settings.rotationPivot);
    }
    if (edit == VoxelRegionEdit::RotateQuarterTurn) {
        Int3 cell{};
        (void)checked(pivot,cell);
        result.rotationPivot = cell;
    }
    const bool extrude = edit == VoxelRegionEdit::Extrude;
    const int factor = settings.amount;
    if (edit == VoxelRegionEdit::ScaleInteger &&
        cells.size() > kMaxVoxelSelection / static_cast<std::size_t>(factor*factor*factor))
        return fail("Scaled region would exceed 500000 selected voxels; select a smaller region");
    std::map<Int3,MaterialId> destination;
    std::set<Int3> anchorsAfter = object.anchors;
    if (!extrude) for (auto cell : cells) anchorsAfter.erase(cell);
    const auto put = [&](Int3 cell, MaterialId material, bool anchored) {
        if (object.voxels->occupied_at(cell) && (extrude || !cells.contains(cell))) {
            result.error = "Destination contains unselected voxels; clear space before editing this region";
            return false;
        }
        destination.emplace(cell,material);
        if (anchored) anchorsAfter.insert(cell);
        if (destination.size() + (extrude ? cells.size() : 0) > kMaxVoxelSelection) {
            result.error = "Result would exceed 500000 selected voxels; select a smaller region";
            return false;
        }
        return true;
    };
    if (extrude) {
        // One furthest selected cell per column: disconnected cells and mixed materials have
        // a deterministic frontier, and no interior face generates overlapping new cells.
        std::map<Int3,Int3> frontier;
        for (const auto cell : cells) {
            auto key = coords(cell);
            key[settings.axis] = 0;
            Int3 column{};
            (void)checked(key,column);
            const auto found = frontier.find(column);
            if (found == frontier.end() ||
                (settings.amount > 0 ? coords(cell)[settings.axis] > coords(found->second)[settings.axis]
                                     : coords(cell)[settings.axis] < coords(found->second)[settings.axis]))
                frontier[column] = cell;
        }
        for (const auto& [column,source] : frontier) {
            (void)column;
            for (int layer = 1; layer <= std::abs(settings.amount); ++layer) {
                auto p = coords(source);
                p[settings.axis] += settings.amount > 0 ? layer : -layer;
                Int3 cell{};
                if (!checked(p,cell)) return fail("Extrusion would exceed the integer voxel grid");
                if (!put(cell,object.voxels->material_at(source),false)) return fail(result.error);
            }
        }
    } else {
        for (const auto source : cells) {
            const auto material = object.voxels->material_at(source);
            const bool anchored = object.anchors.contains(source);
            auto p = coords(source);
            if (edit == VoxelRegionEdit::RotateQuarterTurn) {
                // Cyclic axis pairs give right-handed positive turns: Y/Z, Z/X, X/Y.
                const int u = (settings.axis+1)%3, v = (settings.axis+2)%3;
                const auto a = p[u]-pivot[u], b = p[v]-pivot[v];
                p[u] = pivot[u] - settings.amount*b;
                p[v] = pivot[v] + settings.amount*a;
                Int3 cell{};
                if (!checked(p,cell)) return fail("Rotation would exceed the integer voxel grid");
                if (!put(cell,material,anchored)) return fail(result.error);
            } else {
                for (int axis = 0; axis < 3; ++axis) p[axis] = lo[axis] + (p[axis]-lo[axis])*factor;
                for (int x = 0; x < factor; ++x)
                    for (int y = 0; y < factor; ++y)
                        for (int z = 0; z < factor; ++z) {
                            Int3 cell{};
                            if (!checked({p[0]+x,p[1]+y,p[2]+z},cell))
                                return fail("Scaling would exceed the integer voxel grid");
                            if (!put(cell,material,anchored)) return fail(result.error);
                        }
            }
        }
    }
    std::map<Int3,MaterialId> final;
    if (!extrude) for (auto cell : cells) final[cell] = kAirMaterial;
    for (const auto& [cell,material] : destination) {
        final[cell] = material;
        result.selection.insert(cell);
    }
    if (extrude) result.selection.insert(cells.begin(),cells.end());
    std::vector<VoxelBooleanChange> changes;
    for (const auto& [cell,material] : final) {
        const auto before = object.voxels->material_at(cell);
        if (before != material) changes.push_back({cell,before,material});
    }
    if (changes.empty() && anchorsAfter == object.anchors) return fail("Region is unchanged by this rotation");
    const std::string label = extrude ? "Extrude selected voxels" :
        edit == VoxelRegionEdit::RotateQuarterTurn ? "Rotate selected voxels 90 degrees" : "Scale selected voxels";
    result.command = std::make_unique<ApplyVoxelBooleanCommand>(object.id,std::move(changes),
        std::vector<Int3>(object.anchors.begin(),object.anchors.end()),
        std::vector<Int3>(anchorsAfter.begin(),anchorsAfter.end()),label);
    return result;
}


namespace {
EditorObject conversion_sibling(const EditorObject& source, EditorObjectId id, std::string suffix) {
    EditorObject result(id,source.name+suffix);
    result.transform = source.transform;
    result.voxelScale = source.voxelScale;
    result.voxelSizeMeters = source.voxelSizeMeters;
    result.flags = source.flags; result.flags.locked = false;
    result.parent = source.prefabLink ? std::nullopt : source.parent;
    result.tags = source.tags; result.groups = source.groups; result.layer = source.layer;
    for (const auto& component : source.components)
        if (component.type == "dve.membership" || component.type == "dve.tags") result.components.push_back(component);
    return result;
}
}

GeometryConversionBuild polygon_to_voxels(const EditorObject& source, EditorObjectId outputId,
    GeometryConversionSettings settings, const std::vector<MaterialId>& paletteSlots) {
    GeometryConversionBuild result;
    if (!source.polygon || source.flags.locked || !source.flags.visible) {
        result.error = "Select a visible unlocked polygon mesh"; return result;
    }
    if (!std::isfinite(settings.voxelSizeMeters) || settings.voxelSizeMeters < 0.005F ||
        settings.voxelSizeMeters > 10.0F || !valid_voxel_metric(source.voxelScale)) {
        result.error = "Use a finite local voxel size between 0.005 and 10 metres"; return result;
    }
    if (settings.mode != VoxelizationMode::Solid && settings.mode != VoxelizationMode::Shell &&
        settings.mode != VoxelizationMode::SurfaceOnly) { result.error = "Invalid voxelization mode"; return result; }
    const auto& mesh = *editor_surface_mesh(source);
    if (const auto valid = validate_polygon_asset(mesh); !valid) { result.error = valid.message; return result; }
    if (mesh.indices.size()/3 > 500'000 || mesh.vertices.size() > 1'000'000) {
        result.error = "Convert a smaller mesh (at most 500000 triangles and 1000000 vertices)"; return result;
    }
    std::set<MaterialId> slots(paletteSlots.begin(),paletteSlots.end());
    if (slots.size() != paletteSlots.size() || slots.empty() || slots.contains(kAirMaterial)) {
        result.error = "No free palette slots; use fewer materials"; return result;
    }
    const float size = settings.voxelSizeMeters;
    std::uint64_t cells = 1;
    const std::array<double,3> lo{mesh.bounds.minimum.x,mesh.bounds.minimum.y,mesh.bounds.minimum.z};
    const std::array<double,3> hi{mesh.bounds.maximum.x,mesh.bounds.maximum.y,mesh.bounds.maximum.z};
    for (unsigned axis = 0; axis < 3; ++axis) {
        const double a = std::floor(lo[axis]/size)-1, b = std::floor(hi[axis]/size)+1;
        if (!std::isfinite(a) || !std::isfinite(b) || a < double(kInt3Min.x)+kBrickDim ||
            b > double(kInt3Max.x)-kBrickDim || b < a || b-a+1 > double(kMaxConversionWorkingCells)) {
            result.error = "Conversion bounds exceed the supported voxel grid"; return result;
        }
        const auto extent = static_cast<std::uint64_t>(b-a+1);
        if (cells > kMaxConversionWorkingCells/extent) {
            result.error = "Conversion exceeds 2000000 working cells; use a larger voxel size"; return result;
        }
        cells *= extent;
    }
    try {
        ImportedScene scene; scene.name = source.name;
        for (const auto& image : mesh.images)
            scene.images.push_back({image.name,image.mimeType,image.width,image.height,image.rgba8});
        for (const auto& sampler : mesh.samplers)
            scene.samplers.push_back({sampler.wrapS,sampler.wrapT,sampler.minFilter,sampler.magFilter});
        for (const auto& texture : mesh.textures)
            scene.textures.push_back({texture.name,texture.imageIndex,texture.samplerIndex});
        for (std::size_t i = 0; i < mesh.materials.size(); ++i) {
            const auto& material = mesh.materials[i];
            ImportedMaterial imported;
            imported.name = std::to_string(i)+":"+material.name;
            imported.baseColorFactor = material.baseColor;
            imported.emissiveFactor = material.emissive;
            imported.metallicFactor = material.metallic;
            imported.roughnessFactor = material.roughness;
            imported.alphaMode = material.blendMode == MaterialBlendMode::Masked ? ImportedAlphaMode::Mask :
                material.blendMode == MaterialBlendMode::Translucent || material.transparent ? ImportedAlphaMode::Blend :
                ImportedAlphaMode::Opaque;
            if (i < mesh.materialBindings.size()) {
                const auto& binding = mesh.materialBindings[i];
                imported.baseColorTexture = binding.baseColor.texture;
                imported.alphaCutoff = binding.alphaCutoff;
                imported.doubleSided = binding.doubleSided;
            }
            // The voxelizer samples UV0; each triangle below supplies its material's selected
            // base-color channel there. Other shader/detail/normal maps are not baked.
            scene.materials.push_back(std::move(imported));
        }
        ImportedMesh importedMesh; importedMesh.name = source.name;
        std::uint64_t triangleWork = 0; // Bound raster work as well as the dense grid allocation.
        std::map<std::pair<std::uint32_t,std::uint32_t>,std::uint32_t> vertexMap;
        for (const auto& submesh : mesh.submeshes) {
            const auto channel = submesh.materialIndex < mesh.materialBindings.size() ?
                mesh.materialBindings[submesh.materialIndex].baseColor.texcoord : 0U;
            for (std::uint32_t at = submesh.firstIndex; at < submesh.firstIndex+submesh.indexCount; at += 3) {
                ImportedTriangle triangle; triangle.materialIndex = submesh.materialIndex;
                for (unsigned corner = 0; corner < 3; ++corner) {
                    const auto index = mesh.indices[at+corner];
                    const auto key = std::make_pair(index,channel);
                    auto [found,inserted] = vertexMap.try_emplace(key,static_cast<std::uint32_t>(importedMesh.vertices.size()));
                    if (inserted) {
                        const auto& vertex = mesh.vertices[index];
                        importedMesh.vertices.push_back({vertex.position,vertex.normal,
                            channel == 1 ? vertex.texcoord1 : vertex.texcoord,vertex.color,vertex.texcoord1});
                    }
                    triangle.indices[corner] = found->second;
                }
                std::uint64_t work = 1;
                for (unsigned axis = 0; axis < 3; ++axis) {
                    double minimum = std::numeric_limits<double>::infinity(), maximum = -minimum;
                    for (const auto index : triangle.indices) {
                        const auto p = importedMesh.vertices[index].position;
                        const double coordinate = axis == 0 ? p.x : axis == 1 ? p.y : p.z;
                        minimum = std::min(minimum,coordinate); maximum = std::max(maximum,coordinate);
                    }
                    const auto extent = static_cast<std::uint64_t>(std::floor(maximum/size)-std::floor(minimum/size)+3);
                    if (work > 16'000'000/extent) { result.error = "Triangle raster work exceeds conversion budget; use a larger voxel size"; return result; }
                    work *= extent;
                }
                if (triangleWork > 16'000'000-work) { result.error = "Triangle raster work exceeds conversion budget; use a larger voxel size"; return result; }
                triangleWork += work;
                importedMesh.triangles.push_back(triangle);
            }
        }
        scene.meshes.push_back(std::move(importedMesh));
        ImportedNode node; node.name = source.name; node.mesh = 0;
        scene.nodes.push_back(std::move(node)); scene.roots.push_back(0);
        VoxelizeSettings cook;
        cook.objectId = outputId; cook.voxelSizeMeters = size; cook.mode = settings.mode;
        cook.maximumWorkingVoxels = kMaxConversionWorkingCells;
        cook.maximumPaletteMaterials = std::min<std::uint32_t>(256,static_cast<std::uint32_t>(paletteSlots.size()+1));
        auto cooked = voxelize_scene(scene,cook);
        for (const auto& diagnostic : cooked.diagnostics) {
            if (diagnostic.severity == ImportDiagnostic::Severity::Error) { result.error = diagnostic.message; return result; }
            if (diagnostic.severity == ImportDiagnostic::Severity::Warning) result.note += diagnostic.message+" ";
        }
        if (cooked.object.occupied_voxel_count() == 0 || cooked.materials.size() > paletteSlots.size()+1) {
            result.error = "Conversion produced no cells or exceeded the available palette"; return result;
        }
        auto object = conversion_sibling(source,outputId," (voxels)");
        object.voxelSizeMeters = size;
        object.voxels = std::make_unique<VoxelObject>(outputId);
        for (std::size_t localId = 1; localId < cooked.materials.size(); ++localId) {
            auto material = cooked.materials[localId];
            for (std::size_t original = 0; original < mesh.materials.size(); ++original)
                if (material.name.starts_with(std::to_string(original)+":")) {
                    auto full = mesh.materials[original];
                    full.name = mesh.materials[original].name+" (voxel)";
                    full.baseColor = material.baseColor;
                    full.transparent = material.transparent;
                    full.layers.clear(); // Layer stacks need their own baking, not copied palette references.
                    material = std::move(full); break;
                }
            object.voxelMaterials.emplace(paletteSlots[localId-1],std::move(material));
        }
        for_each_occupied(cooked.object,[&](Int3 cell,MaterialId material) {
            object.voxels->set_voxel(cell,paletteSlots[static_cast<std::size_t>(material)-1]);
        });
        result.note += "Base color/alpha and vertex colors are sampled; shader detail and normal maps are not baked.";
        result.object = std::move(object);
    } catch (const std::exception& exception) { result.error = exception.what(); }
    return result;
}

GeometryConversionBuild voxels_to_polygon(const EditorObject& source, EditorObjectId outputId,
                                          const EditorMaterialLibrary& materials) {
    GeometryConversionBuild result;
    if (auto problem = object_problem(source); !problem.empty()) { result.error = problem; return result; }
    if (!source.flags.visible || !valid_voxel_metric(source.voxelScale) ||
        !std::isfinite(source.voxelSizeMeters) || source.voxelSizeMeters <= 0) {
        result.error = "Select visible voxels with a valid grid size"; return result;
    }
    if (source.voxels->occupied_voxel_count() == 0 || source.voxels->occupied_voxel_count() > 250'000) {
        result.error = "Convert an occupied object with at most 250000 cells"; return result;
    }
    constexpr int corners[6][4][3] = {
        {{1,0,0},{1,1,0},{1,1,1},{1,0,1}}, {{0,0,0},{0,0,1},{0,1,1},{0,1,0}},
        {{0,1,0},{0,1,1},{1,1,1},{1,1,0}}, {{0,0,0},{1,0,0},{1,0,1},{0,0,1}},
        {{0,0,1},{1,0,1},{1,1,1},{0,1,1}}, {{0,0,0},{0,1,0},{1,1,0},{1,0,0}}
    };
    constexpr Float2 uv[4] = {{0,0},{1,0},{1,1},{0,1}};
    CookedPolygonAsset mesh; mesh.objectId = outputId;
    std::map<MaterialId,std::vector<std::uint32_t>> groups;
    std::size_t faceCount = 0;
    bool tooLarge = false;
    for_each_occupied(*source.voxels,[&](Int3 cell,MaterialId material) {
        if (tooLarge) return;
        for (unsigned face = 0; face < 6; ++face) {
            const auto delta = kFaces[face];
            const std::int64_t nx = std::int64_t(cell.x)+delta.x, ny = std::int64_t(cell.y)+delta.y, nz = std::int64_t(cell.z)+delta.z;
            const bool valid = nx >= kInt3Min.x && nx <= kInt3Max.x && ny >= kInt3Min.y && ny <= kInt3Max.y &&
                               nz >= kInt3Min.z && nz <= kInt3Max.z;
            if (valid && source.voxels->occupied_at({static_cast<std::int32_t>(nx),static_cast<std::int32_t>(ny),
                                                  static_cast<std::int32_t>(nz)})) continue;
            if (++faceCount > kMaxConversionFaces) { tooLarge = true; return; }
            const auto base = static_cast<std::uint32_t>(mesh.vertices.size());
            for (unsigned corner = 0; corner < 4; ++corner) {
                PolygonVertex vertex;
                vertex.position = {float(double(cell.x)+corners[face][corner][0])*source.voxelSizeMeters,
                    float(double(cell.y)+corners[face][corner][1])*source.voxelSizeMeters,
                    float(double(cell.z)+corners[face][corner][2])*source.voxelSizeMeters};
                vertex.normal = {float(delta.x),float(delta.y),float(delta.z)};
                vertex.texcoord = vertex.texcoord1 = uv[corner];
                mesh.vertices.push_back(vertex);
            }
            auto& indices = groups[material];
            indices.insert(indices.end(),{base,base+1,base+2,base,base+2,base+3});
        }
    });
    if (tooLarge) { result.error = "Surface exceeds 250000 exposed faces; split the object first"; return result; }
    for (const auto& [materialId,indices] : groups) {
        VoxelMaterialDefinition definition;
        if (const auto owned = source.voxelMaterials.find(materialId); owned != source.voxelMaterials.end()) definition = owned->second;
        else if (const auto* entry = materials.find(materialId)) definition = entry->definition;
        else { definition.name = "Material "+std::to_string(materialId); definition.baseColor = {0.65F,0.68F,0.72F,1}; }
        definition.layers.clear();
        const auto materialIndex = static_cast<std::uint32_t>(mesh.materials.size());
        mesh.materials.push_back(std::move(definition)); mesh.materialBindings.emplace_back();
        mesh.submeshes.push_back({mesh.materials.back().name,static_cast<std::uint32_t>(mesh.indices.size()),
            static_cast<std::uint32_t>(indices.size()),materialIndex});
        mesh.indices.insert(mesh.indices.end(),indices.begin(),indices.end());
    }
    refresh_editor_polygon(mesh,false);
    if (const auto valid = validate_polygon_asset(mesh); !valid) { result.error = valid.message; return result; }
    auto object = conversion_sibling(source,outputId," (polygon)");
    object.polygon = std::move(mesh);
    result.object = std::move(object);
    result.note = "Block surface; UV0/UV1 repeat a unit square per face. Materials are grouped; no smoothing or UV packing.";
    return result;
}

VoxelClipboard copy_voxels(const EditorObject& object, const std::set<Int3>& cells) {
    VoxelClipboard clipboard;
    clipboard.name = object.name;
    clipboard.voxelSizeMeters = object.voxelSizeMeters;
    clipboard.voxelScale = object.voxelScale;
    clipboard.materials = object.voxelMaterials;
    clipboard.transform = object.transform;
    clipboard.parent = object.parent;
    clipboard.flags = object.flags;
    clipboard.flags.locked = false;
    if (!object.voxels) return clipboard;
    for (const Int3 cell : cells) {
        const MaterialId material = object.voxels->material_at(cell);
        if (material == kAirMaterial) continue;
        clipboard.cells.push_back({cell, material});
        if (object.anchors.contains(cell)) clipboard.anchors.push_back(cell);
    }
    return clipboard;
}

EditorObject make_object_from_voxels(EditorObjectId id, const VoxelClipboard& clipboard, std::string name) {
    EditorObject object(id, std::move(name));
    object.voxelSizeMeters = clipboard.voxelSizeMeters;
    object.voxelScale = clipboard.voxelScale;
    object.voxelMaterials = clipboard.materials;
    object.transform = clipboard.transform;
    object.parent = clipboard.parent;
    object.flags = clipboard.flags;
    for (const SparseVoxelStateEntry& cell : clipboard.cells) (void)object.voxels->set_voxel(cell.voxel, cell.material);
    object.anchors.insert(clipboard.anchors.begin(), clipboard.anchors.end());
    return object;
}

ExtractVoxelsBuild build_extract_voxels_command(EditorDocument& document, EditorObjectId id, const std::set<Int3>& cells) {
    ExtractVoxelsBuild build;
    const EditorObject* object = document.find_object(id);
    if (!object) { build.error = "The object no longer exists"; return build; }
    if (std::string problem = object_problem(*object); !problem.empty()) { build.error = problem; return build; }
    // Same rules as Separate Islands and Slice's Separate (#97, #100): a prefab instance's part
    // would silently diverge from the prefab, and parts of an attached object would all hang off
    // the same socket.
    if (object->prefabLink) { build.error = object->name + " is a prefab instance; unpack it first"; return build; }
    if (object->attachment) { build.error = object->name + " is attached to a socket; detach it first"; return build; }
    const VoxelClipboard clipboard = copy_voxels(*object, cells);
    if (clipboard.cells.empty()) { build.error = "No voxels selected"; return build; }
    if (clipboard.cells.size() == object->voxels->occupied_voxel_count()) {
        build.error = "Every voxel is selected; that is the whole object (use Duplicate or Separate Islands)";
        return build;
    }
    std::string error;
    auto remove = build_delete_voxels_command(*object, cells, &error);
    if (!remove) { build.error = error; return build; }
    build.newObject = document.allocate_object_id();
    EditorObject part = make_object_from_voxels(build.newObject, clipboard, object->name + " (extract)");
    // Like Separate Islands' pieces: keep flags, tags, groups, layer and membership; geometry no
    // longer matches an import source or prefab, so those (and other components) stay behind.
    part.flags = object->flags;
    part.tags = object->tags;
    part.groups = object->groups;
    part.layer = object->layer;
    for (const Component& component : object->components)
        if (component.type == "dve.membership" || component.type == "dve.tags") part.components.push_back(component);
    build.command = std::make_unique<CompoundCommand>("Extract voxels");
    build.command->add(std::move(remove));
    build.command->add(std::make_unique<AddObjectCommand>(std::move(part), "Extracted voxels"));
    return build;
}

} // namespace dve::editor
