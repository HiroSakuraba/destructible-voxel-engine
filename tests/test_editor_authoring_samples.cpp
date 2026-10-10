#include "../docs/examples/editor_authoring_samples.hpp"
#include <iostream>
#include <stdexcept>
#define CHECK(x)                                                                                             \
    do {                                                                                                     \
        if (!(x))                                                                                            \
            throw std::runtime_error(#x);                                                                    \
    } while (false)
using namespace dve;
using namespace dve::editor::samples;
bool near(Float3 a, Float3 b) { return length(subtract(a, b)) < 1e-4F; }
int main() {
    try {
        const auto a = from_rigid(
            make_rigid_transform({2, 3, 4}, quaternion_from_axis_angle({0, 0, 1}, 0.7F)), {2, 3, 4});
        CHECK(near(inverse(a)->point(a.point({1, 2, 3})), {1, 2, 3}));
        CHECK(std::abs(dot(*normal_to_world(a, {0, 1, 0}), a.vector({1, 0, 0}))) < 1e-4F);
        CHECK(!inverse(Affine{{0, 0, 0}, {0, 1, 0}, {0, 0, 1}, {}}));
        const auto scaled = scale_world_about(a, {1, 1, 1}, {1.7F, 0.8F, 2.3F});
        CHECK(scaled);
        CHECK(near(scaled->translation,
                   add({1, 1, 1}, {(a.translation.x - 1) * 1.7F, (a.translation.y - 1) * 0.8F,
                                   (a.translation.z - 1) * 2.3F})));
        const Ray world{{5, 6, 7}, normalize(Float3{1, 2, 3})};
        const auto local = ray_to_local(*scaled, world);
        CHECK(local);
        CHECK(near(scaled->point(add(local->origin, multiply(local->direction, 10))),
                   add(world.origin, multiply(world.direction, 10))));
        MeshBuilder builder;
        auto v0 = builder.add_vertex({0, 0, 0}), v1 = builder.add_vertex({1, 0, 0}),
             v2 = builder.add_vertex({0, 1, 0});
        auto face = builder.add_triangle(v0, v1, v2, 7);
        CHECK(face);
        EditableMesh mesh = std::move(builder).finish();
        const auto originalHash = editable_mesh_content_hash(mesh);
        MeshGesture gesture;
        MeshSelection selection;
        selection.faces.insert(*face);
        CHECK(gesture.begin(mesh, selection));
        CHECK(gesture.transform(Affine{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {2, 3, 4}}));
        CHECK(editable_mesh_content_hash(mesh) == originalHash);
        CHECK(near(gesture.preview().vertex(v0)->position, {2, 3, 4}));
        gesture.cancel();
        CHECK(editable_mesh_content_hash(mesh) == originalHash);
        CHECK(gesture.begin(mesh, selection));
        CHECK(gesture.transform(Affine{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {2, 3, 4}}));
        CHECK(gesture.commit(mesh));
        CHECK(mesh.face(*face)->material == 7);
        CHECK(near(mesh.vertex(v0)->position, {2, 3, 4}));
        mesh = gesture.before();
        CHECK(editable_mesh_content_hash(mesh) == originalHash);
        CHECK(gesture.begin(mesh, selection));
        mesh.vertex(v0)->position = {0, 0, 1};
        CHECK(!gesture.commit(mesh));
        selection.faces.clear();
        selection.vertices.insert(VertexHandle{v1.index, v1.generation + 1});
        CHECK(!gesture.begin(mesh, selection));
        std::cout << "Authoring samples checks passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
