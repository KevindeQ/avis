#include "avis/scene/scene.h"

#include "labels.h"

#include "avis/detail/check.h"
#include "avis/scene/components/driver.h"
#include "avis/scene/components/viewing.h"
#include "avis/scene/error.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <string_view>
#include <type_traits>
#include <utility>

namespace avis::scene
{
    namespace
    {
        constexpr std::string_view logger_name = "scene";

        // The first id of the helper range: source 0xFF in bits 63–56 (types.h)
        constexpr object_id first_helper_id = (helper_source << 56) + 1;

        // A Driver entity's node has unit scale (§4.3)
        constexpr float unit_scale_tolerance = 1e-4f;

        const transform& identity_transform() noexcept
        {
            static const transform identity{};
            return identity;
        }

        const Eigen::Affine3f& identity_affine() noexcept
        {
            static const Eigen::Affine3f identity = Eigen::Affine3f::Identity();
            return identity;
        }

        template<typename Component>
        constexpr std::string_view owned_name() noexcept
        {
            if constexpr (std::is_same_v<Component, components::identity>)
            {
                return "identity";
            }
            else if constexpr (std::is_same_v<Component, components::node_reference>)
            {
                return "node_reference";
            }
            else
            {
                return "previous_world";
            }
        }

        void retain_valid(asset_cache& assets, const mesh_handle mesh) noexcept
        {
            if (mesh.valid())
            {
                assets.retain(mesh);
            }
        }

        void release_valid(asset_cache& assets, const mesh_handle mesh) noexcept
        {
            if (mesh.valid())
            {
                assets.release(mesh);
            }
        }

        void retain_valid(asset_cache& assets, const std::vector<material_handle>& materials) noexcept
        {
            for (const material_handle material : materials)
            {
                if (material.valid())
                {
                    assets.retain(material);
                }
            }
        }

        // Sorts and removes duplicates, in place
        template<typename Handle>
        void sort_unique(std::vector<Handle>& handles) noexcept
        {
            std::ranges::sort(handles);
            const auto duplicates = std::ranges::unique(handles);
            handles.erase(duplicates.begin(), duplicates.end());
        }

        // The entry for `handle` in a table sorted by handle; nullptr when it isn't there
        template<typename Entry, typename Handle>
        const Entry* find_entry(const std::vector<Entry>& table, const Handle handle) noexcept
        {
            const auto found = std::ranges::lower_bound(table, handle, {}, &Entry::handle);
            return found != table.end() && found->handle == handle ? &*found : nullptr;
        }

        void release_valid(asset_cache& assets, const std::vector<material_handle>& materials) noexcept
        {
            for (const material_handle material : materials)
            {
                if (material.valid())
                {
                    assets.release(material);
                }
            }
        }
    } // namespace

    // Allows writes to the owned components while it lives. `destroying` marks scene's own destruction of entities,
    // during which losing `represents` records no change.
    class scene::owner_scope
    {
    public:
        explicit owner_scope(scene& owner, const bool destroying = false) noexcept :
            owner_{ owner },
            previous_destroying_{ owner.destroying_ }
        {
            ++owner_.owner_writes_;
            owner_.destroying_ = owner_.destroying_ || destroying;
        }

        ~owner_scope()
        {
            --owner_.owner_writes_;
            owner_.destroying_ = previous_destroying_;
        }

        owner_scope(const owner_scope&) = delete;
        owner_scope& operator=(const owner_scope&) = delete;

    private:
        scene& owner_;
        bool previous_destroying_;
    };

    scene::scene(asset_cache& assets) :
        assets_{ assets },
        owner_thread_{ std::this_thread::get_id() },
        next_helper_id_{ first_helper_id },
        labels_{ std::make_unique<detail::label_formatter>() }
    {
        using namespace components;

        registry_.on_construct<identity>().connect<&scene::on_identity_construct>(*this);
        registry_.on_destroy<identity>().connect<&scene::on_identity_destroy>(*this);
        registry_.on_destroy<identity>().connect<&scene::on_entity_destroy>(*this);
        registry_.on_construct<represents>().connect<&scene::on_represents_construct>(*this);
        registry_.on_destroy<represents>().connect<&scene::on_represents_destroy>(*this);
        registry_.on_construct<mesh>().connect<&scene::on_draw_set_change>(*this);
        registry_.on_destroy<mesh>().connect<&scene::on_draw_set_change>(*this);
        registry_.on_construct<material>().connect<&scene::on_draw_set_change>(*this);
        registry_.on_destroy<material>().connect<&scene::on_draw_set_change>(*this);
        registry_.on_construct<mesh>().connect<&scene::on_mesh_construct>(*this);
        registry_.on_update<mesh>().connect<&scene::on_mesh_update>(*this);
        registry_.on_destroy<mesh>().connect<&scene::on_mesh_destroy>(*this);
        registry_.on_construct<material>().connect<&scene::on_material_construct>(*this);
        registry_.on_update<material>().connect<&scene::on_material_update>(*this);
        registry_.on_destroy<material>().connect<&scene::on_material_destroy>(*this);
        registry_.on_construct<label>().connect<&scene::on_label_construct>(*this);
        registry_.on_destroy<label>().connect<&scene::on_label_destroy>(*this);
        registry_.on_construct<trail>().connect<&scene::on_trail_construct>(*this);
        registry_.on_destroy<trail>().connect<&scene::on_trail_destroy>(*this);

#ifndef NDEBUG
        registry_.on_construct<identity>().connect<&scene::on_owned_write<identity>>(*this);
        registry_.on_update<identity>().connect<&scene::on_owned_write<identity>>(*this);
        registry_.on_destroy<identity>().connect<&scene::on_owned_write<identity>>(*this);
        registry_.on_construct<node_reference>().connect<&scene::on_owned_write<node_reference>>(*this);
        registry_.on_update<node_reference>().connect<&scene::on_owned_write<node_reference>>(*this);
        registry_.on_destroy<node_reference>().connect<&scene::on_owned_write<node_reference>>(*this);
        registry_.on_construct<previous_world>().connect<&scene::on_owned_write<previous_world>>(*this);
        registry_.on_update<previous_world>().connect<&scene::on_owned_write<previous_world>>(*this);
        registry_.on_destroy<previous_world>().connect<&scene::on_owned_write<previous_world>>(*this);
#endif
    }

    scene::~scene()
    {
        destroy_all();
    }

    // ---- entities ----

    result<entity> scene::create_entity(const entity_description& description)
    {
        check_thread("create_entity");

        object_id id = description.id;
        if (id == 0)
        {
            id = allocate_helper_id();
        }
        else if (ids_.contains(id))
        {
            return std::unexpected(
                make_error(error_kind::duplicate_id, std::format("create_entity: id {:#x} already exists", id)));
        }

        node_handle parent_node;
        if (description.parent != entt::null)
        {
            if (!description.spatial)
            {
                return std::unexpected(make_error(
                    error_kind::invalid_parent,
                    std::format("create_entity: the non-spatial entity {:#x} can't have a parent", id)));
            }
            const auto* parent_reference =
                alive(description.parent) ? registry_.try_get<components::node_reference>(description.parent) : nullptr;
            if (parent_reference == nullptr)
            {
                return std::unexpected(make_error(
                    error_kind::invalid_parent,
                    std::format("create_entity: the parent of {:#x} isn't alive or has no node", id)));
            }
            parent_node = parent_reference->node;
        }

        ids_.reserve(ids_.size() + 1);
        const entity created = registry_.create();
        {
            const owner_scope scope{ *this };
            registry_.emplace<components::identity>(
                created, components::identity{ .id = id, .name = description.name, .category = description.category });
            if (description.spatial)
            {
                const node_handle node = graph_.create_node(created, parent_node, description.local);
                registry_.emplace<components::node_reference>(created, node);
            }
        }
        ids_.emplace(id, created);

        record_change(tree_change_kind::added, created);
        ++scene_version_;
        tick_active_ = true;
        if (description.spatial)
        {
            note_transform_change();
        }
        return created;
    }

    std::vector<destroyed_entity> scene::destroy_entity(const entity entity)
    {
        check_thread("destroy_entity");
        if (!check_alive(entity, "destroy_entity"))
        {
            return {};
        }

        // One change for the root: the GUI drops the subtree
        record_change(tree_change_kind::removed, entity);

        std::vector<avis::scene::entity> payloads;
        if (const auto* reference = registry_.try_get<components::node_reference>(entity))
        {
            graph_.destroy_node(reference->node, payloads); // children first
        }
        else
        {
            payloads.push_back(entity);
        }

        std::vector<destroyed_entity> destroyed;
        destroyed.reserve(payloads.size());
        const owner_scope scope{ *this, true };
        for (const avis::scene::entity payload : payloads)
        {
            const object_id id = id_of(payload);
            if (const auto found = ids_.find(id); found != ids_.end() && found->second == payload)
            {
                ids_.erase(found);
            }
            destroyed.push_back({ .handle = payload, .id = id });
            registry_.destroy(payload);
        }

        ++scene_version_;
        tick_active_ = true;
        return destroyed;
    }

    void scene::clear()
    {
        check_thread("clear");

        // One removed change per root of the tree: graph roots and non-spatial entities
        for (const entity entity : registry_.view<components::identity>())
        {
            const auto* reference = registry_.try_get<components::node_reference>(entity);
            if (reference == nullptr || !graph_.parent(reference->node).valid())
            {
                record_change(tree_change_kind::removed, entity);
            }
        }

        destroy_all();
        ids_.clear();
        next_helper_id_ = first_helper_id;
        ++scene_version_;
        tick_active_ = true;
        transforms_pending_ = false; // nothing left to propagate
    }

    bool scene::alive(const entity entity) const noexcept
    {
        check_thread("alive");
        return registry_.valid(entity) && registry_.all_of<components::identity>(entity);
    }

    std::optional<entity> scene::find(const object_id id) const noexcept
    {
        check_thread("find");
        if (const auto found = ids_.find(id); found != ids_.end() && registry_.valid(found->second))
        {
            return found->second;
        }
        return std::nullopt;
    }

    entity scene::selection_target(const entity entity) const noexcept
    {
        check_thread("selection_target");

        // A chain longer than the number of entities is a cycle
        const std::size_t limit = ids_.size() + 1;
        std::size_t steps = 0;
        avis::scene::entity current = entity;
        while (const auto* owner =
                   registry_.valid(current) ? registry_.try_get<components::represents>(current) : nullptr)
        {
            if (!registry_.valid(owner->owner))
            {
                break;
            }
            if (!AVIS_CHECK(logger_name, ++steps <= limit, "selection_target: `represents` forms a cycle"))
            {
                return entity;
            }
            current = owner->owner;
        }
        return current;
    }

    object_id scene::allocate_helper_id() noexcept
    {
        check_thread("allocate_helper_id");
        while (ids_.contains(next_helper_id_))
        {
            ++next_helper_id_;
        }
        AVIS_CHECK(logger_name, is_helper(next_helper_id_), "allocate_helper_id: the helper range is exhausted");
        return next_helper_id_++;
    }

    entt::registry& scene::registry() noexcept
    {
        check_thread("registry");
        return registry_;
    }

    const entt::registry& scene::registry() const noexcept
    {
        check_thread("registry");
        return registry_;
    }

    asset_cache& scene::assets() noexcept
    {
        check_thread("assets");
        return assets_;
    }

    // ---- hierarchy and transforms ----

    result<void> scene::set_parent(const entity child, const entity parent, const bool keep_world)
    {
        check_thread("set_parent");

        const auto* reference = alive(child) ? registry_.try_get<components::node_reference>(child) : nullptr;
        if (reference == nullptr)
        {
            return std::unexpected(
                make_error(error_kind::invalid_parent, "set_parent: the entity isn't alive or has no node"));
        }

        node_handle parent_node; // null: the entity becomes a root
        if (parent != entt::null)
        {
            const auto* parent_reference =
                alive(parent) ? registry_.try_get<components::node_reference>(parent) : nullptr;
            if (parent_reference == nullptr)
            {
                return std::unexpected(make_error(
                    error_kind::invalid_parent,
                    std::format("set_parent: the new parent of {:#x} isn't alive or has no node", id_of(child))));
            }
            parent_node = parent_reference->node;
        }

        // invalid_parent and cycle come from the scene graph with the same kinds
        if (result<void> moved = graph_.reparent(reference->node, parent_node, keep_world); !moved)
        {
            return moved;
        }

        record_change(tree_change_kind::moved, child);
        ++scene_version_;
        tick_active_ = true;
        note_transform_change();
        return {};
    }

    void scene::set_local(const entity entity, const transform& local, const bool discontinuity)
    {
        check_thread("set_local");
        const node_handle node = node_of(entity, "set_local");
        if (!node.valid())
        {
            return;
        }

#ifndef NDEBUG
        if (registry_.all_of<components::driver_entity>(entity))
        {
            AVIS_CHECK(
                logger_name,
                ((local.scale.array() - 1.0f).abs() <= unit_scale_tolerance).all(),
                std::format(
                    "set_local: the Driver entity {:#x} has scale ({}, {}, {}); its node must have unit scale, and the "
                    "extent goes to its representation (08 §4.3)",
                    id_of(entity),
                    local.scale.x(),
                    local.scale.y(),
                    local.scale.z()));
        }
#endif

        graph_.set_local(node, local, discontinuity);
        tick_active_ = true;
        note_transform_change();
    }

    const transform& scene::local(const entity entity) const
    {
        check_thread("local");
        const node_handle node = node_of(entity, "local");
        return node.valid() ? graph_.local(node) : identity_transform();
    }

    const Eigen::Affine3f& scene::world(const entity entity) const
    {
        check_thread("world");
        const node_handle node = node_of(entity, "world");
        return node.valid() ? graph_.world(node) : identity_affine();
    }

    Eigen::Affine3f scene::relative_transform(const entity from, const entity to) const
    {
        check_thread("relative_transform");
        return world(from).inverse(Eigen::Affine) * world(to);
    }

    std::optional<aabb> scene::world_bounds(const entity entity) const
    {
        check_thread("world_bounds");
        if (!check_alive(entity, "world_bounds"))
        {
            return std::nullopt;
        }
        const auto* reference = registry_.try_get<components::node_reference>(entity);
        if (reference == nullptr)
        {
            return std::nullopt; // non-spatial: nothing is drawn
        }

        aabb bounds;
        graph_.for_each_in_subtree(
            reference->node,
            [&](const node_handle node)
            {
                const avis::scene::entity payload = graph_.payload(node);
                const auto* drawn = registry_.try_get<components::mesh>(payload);
                if (drawn == nullptr || !drawn->asset.valid())
                {
                    return;
                }
                const auto* options = registry_.try_get<components::render_options>(payload);
                if (options != nullptr && options->flags & instance_flags::screen_aligned)
                {
                    return; // in pixels: labels (§8.1)
                }

                const versioned_mesh current = assets_.current(drawn->asset);
                if (!current.data)
                {
                    return;
                }
                const std::vector<mesh_primitive>& primitives = current.data->primitives;
                const auto count = static_cast<std::uint32_t>(primitives.size());
                aabb local;
                for (std::uint32_t index = drawn->primitive_first;
                     index < count && index - drawn->primitive_first < drawn->primitive_count;
                     ++index)
                {
                    local.extend(primitives[index].bounds);
                }
                if (!local.empty())
                {
                    bounds.extend(local.transformed(graph_.world(node)));
                }
            });

        if (bounds.empty())
        {
            return std::nullopt;
        }
        return bounds;
    }

    // ---- per tick ----

    void scene::update_world_transforms()
    {
        check_thread("update_world_transforms");
        const auto start = std::chrono::steady_clock::now();
        {
            // A jumped node doesn't interpolate: its previous world is its new one (§4.1)
            const owner_scope scope{ *this };
            graph_.update_transforms(
                [this](const node_handle node)
                {
                    const avis::scene::entity payload = graph_.payload(node);
                    store_previous_world(payload, graph_.world(node));
                    if (!jumped_.contains(payload))
                    {
                        jumped_.push(payload); // for update_trails
                    }
                });
        }
        transforms_pending_ = false;
        tick_active_ = true;
        tick_transform_time_ += std::chrono::steady_clock::now() - start;
    }

    void scene::make_snapshot(snapshot& target)
    {
        check_thread("make_snapshot");
        check_snapshot_order("make_snapshot");
        const auto start = std::chrono::steady_clock::now();

        target.clear();
        update_draw_order();
        collect_assets(target);
        add_instances(target);
        add_lights(target);
        target.environment = environment_;
        target.scene_version = scene_version_;

        last_snapshot_step_ = target.step;
        last_snapshot_instances_ = target.instances.size();
        last_snapshot_lights_ = target.lights.size();
        last_snapshot_time_ = std::chrono::steady_clock::now() - start;
    }

    void scene::end_tick()
    {
        check_thread("end_tick");
#ifndef NDEBUG
        AVIS_CHECK(
            logger_name,
            tick_active_,
            "end_tick: called twice with no change or update_world_transforms in between (08 §9.6)");
#endif
        if (!AVIS_CHECK(
                logger_name,
                !transforms_pending_,
                "end_tick: a transform changed after the last update_world_transforms (08 §9.6)"))
        {
            update_world_transforms(); // so previous_world gets current worlds
        }

        const auto start = std::chrono::steady_clock::now();
        {
            const owner_scope scope{ *this };
            for (const auto [entity, reference] : registry_.view<components::node_reference>().each())
            {
                store_previous_world(entity, graph_.world(reference.node));
            }
        }
        tick_transform_time_ += std::chrono::steady_clock::now() - start;

        last_transform_time_ = std::exchange(tick_transform_time_, std::chrono::nanoseconds{ 0 });
        tick_active_ = false;
        jumped_.clear();
        trails_updated_ = false;
    }

    // ---- environment ----

    void scene::set_environment(const environment_settings& environment)
    {
        check_thread("set_environment");
        environment_ = environment;
    }

    const environment_settings& scene::environment() const noexcept
    {
        check_thread("environment");
        return environment_;
    }

    // ---- UI ----

    std::vector<tree_change> scene::take_tree_changes()
    {
        check_thread("take_tree_changes");
        return std::exchange(tree_changes_, {});
    }

    std::uint64_t scene::scene_version() const noexcept
    {
        check_thread("scene_version");
        return scene_version_;
    }

    reflected_entity scene::reflect(const entity entity) const
    {
        check_thread("reflect");
        if (!check_alive(entity, "reflect"))
        {
            return {};
        }

        const auto& identity = registry_.get<components::identity>(entity);
        reflected_entity reflected{ .id = identity.id, .name = identity.name };
        detail::reflect_components(registry_, entity, reflected.components);
        if (const auto* values = registry_.try_get<components::properties>(entity))
        {
            reflected.properties = values->entries();
        }
        if (const auto* stamp = registry_.try_get<components::update_stamp>(entity))
        {
            reflected.stamp = *stamp;
        }
        reflected.world_bounds = world_bounds(entity);
        return reflected;
    }

    scene_statistics scene::statistics() const
    {
        check_thread("statistics");

        const auto narrow = [](const std::size_t count) { return static_cast<std::uint32_t>(count); };
        const asset_statistics cached = assets_.statistics();
        scene_statistics result{
            .step = last_snapshot_step_,
            .entities = narrow(counters_.entities),
            .nodes = narrow(graph_.size()),
            .instances = narrow(last_snapshot_instances_),
            .lights = narrow(last_snapshot_lights_),
            .meshes = cached.meshes,
            .materials = cached.materials,
            .textures = cached.textures,
            .current_geometry_bytes = cached.current_geometry_bytes,
            .retained_geometry_bytes = cached.retained_geometry_bytes,
            .labels = narrow(counters_.labels),
            .trails = narrow(counters_.trails),
            .trail_samples = narrow(counters_.trail_samples),
            .transform_update_milliseconds = std::chrono::duration<double, std::milli>(last_transform_time_).count(),
            .snapshot_build_milliseconds = std::chrono::duration<double, std::milli>(last_snapshot_time_).count(),
        };
        for (std::size_t category = 0; category < entity_category_count; ++category)
        {
            result.entities_per_category[category] = narrow(counters_.per_category[category]);
        }
        return result;
    }

    // ---- private ----

    bool scene::check_thread([[maybe_unused]] const char* operation) const noexcept
    {
#ifndef NDEBUG
        return AVIS_CHECK(
            logger_name,
            std::this_thread::get_id() == owner_thread_,
            std::format("{}: called from a thread other than the simulation thread", operation));
#else
        return true;
#endif
    }

    bool scene::check_alive(const entity entity, const char* operation) const noexcept
    {
        return AVIS_CHECK(logger_name, alive(entity), std::format("{}: the entity isn't alive", operation));
    }

    object_id scene::id_of(const entity entity) const noexcept
    {
        const auto* identity = registry_.try_get<components::identity>(entity);
        return identity != nullptr ? identity->id : 0;
    }

    object_id scene::parent_id_of(const entity entity) const noexcept
    {
        const auto* reference = registry_.try_get<components::node_reference>(entity);
        if (reference == nullptr)
        {
            return 0;
        }
        const node_handle parent = graph_.parent(reference->node);
        return parent.valid() ? id_of(graph_.payload(parent)) : 0;
    }

    void scene::record_change(const tree_change_kind kind, const entity entity)
    {
        const auto* identity = registry_.try_get<components::identity>(entity);
        if (identity == nullptr)
        {
            return; // not created by scene
        }
        tree_changes_.push_back(
            {
                .kind = kind,
                .id = identity->id,
                .parent = parent_id_of(entity),
                .name = identity->name,
                .category = identity->category,
                .hidden = registry_.all_of<components::represents>(entity),
            });
    }

    void scene::destroy_all()
    {
        const owner_scope scope{ *this, true };
        registry_.clear(); // the on_destroy signals release the assets
        graph_.clear();
        jumped_.clear();
    }

    node_handle scene::node_of(const entity entity, const char* operation) const noexcept
    {
        const auto* reference = alive(entity) ? registry_.try_get<components::node_reference>(entity) : nullptr;
        if (!AVIS_CHECK(
                logger_name, reference != nullptr, std::format("{}: the entity isn't alive or has no node", operation)))
        {
            return {};
        }
        return reference->node;
    }

    void scene::store_previous_world(const entity entity, const Eigen::Affine3f& world)
    {
        // A plain write when it's there, so the steady state fires no signal
        if (auto* previous = registry_.try_get<components::previous_world>(entity))
        {
            previous->world = world;
        }
        else
        {
            registry_.emplace<components::previous_world>(entity, world);
        }
    }

    void scene::note_transform_change() noexcept
    {
        transforms_pending_ = true;
    }

    void scene::check_snapshot_order([[maybe_unused]] const char* operation) const noexcept
    {
#ifndef NDEBUG
        AVIS_CHECK(
            logger_name,
            !transforms_pending_,
            std::format("{}: a transform changed after the last update_world_transforms (08 §9.6)", operation));
#endif
    }

    // ---- snapshots ----

    void scene::update_draw_order()
    {
        if (!draw_order_dirty_ && draw_order_version_ == scene_version_)
        {
            return;
        }

        const auto drawn = registry_.view<components::node_reference, components::mesh, components::material>();
        draw_order_.assign(drawn.begin(), drawn.end());
        std::ranges::sort(
            draw_order_,
            {},
            [this](const avis::scene::entity entity) { return registry_.get<components::identity>(entity).id; });

        draw_order_version_ = scene_version_;
        draw_order_dirty_ = false;
    }

    void scene::collect_assets(snapshot& target)
    {
        mesh_scratch_.clear();
        material_scratch_.clear();
        texture_scratch_.clear();
        for (const avis::scene::entity entity : draw_order_)
        {
            if (const mesh_handle mesh = registry_.get<components::mesh>(entity).asset; mesh.valid())
            {
                mesh_scratch_.push_back(mesh);
            }
            for (const material_handle material : registry_.get<components::material>(entity).materials)
            {
                if (material.valid())
                {
                    material_scratch_.push_back(material);
                }
            }
        }
        sort_unique(mesh_scratch_);
        sort_unique(material_scratch_);

        // The components retain their handles, so each one is alive and has a current version
        for (const mesh_handle mesh : mesh_scratch_)
        {
            if (versioned_mesh current = assets_.current(mesh); current.data)
            {
                target.meshes.push_back({ .handle = mesh, .mesh = std::move(current) });
            }
        }
        for (const material_handle material : material_scratch_)
        {
            versioned_material current = assets_.current(material);
            if (!current.data)
            {
                continue;
            }
            const material_description& description = *current.data;
            for (const texture_handle texture : { description.base_color_texture,
                                                  description.metallic_roughness_texture,
                                                  description.normal_texture,
                                                  description.occlusion_texture,
                                                  description.emissive_texture })
            {
                if (texture.valid())
                {
                    texture_scratch_.push_back(texture);
                }
            }
            target.materials.push_back({ .handle = material, .material = std::move(current) });
        }
        sort_unique(texture_scratch_);
        for (const texture_handle texture : texture_scratch_)
        {
            if (versioned_texture current = assets_.current(texture); current.data)
            {
                target.textures.push_back({ .handle = texture, .texture = std::move(current) });
            }
        }
    }

    void scene::add_instances(snapshot& target)
    {
        static const components::render_options default_options{};

        for (const avis::scene::entity entity : draw_order_)
        {
            const auto& mesh = registry_.get<components::mesh>(entity);
            const versioned_mesh_entry* entry = mesh.asset.valid() ? find_entry(target.meshes, mesh.asset) : nullptr;
            if (entry == nullptr)
            {
                continue; // no mesh to draw
            }

            // The range, clamped to the current version's primitives
            const auto primitives = static_cast<std::uint32_t>(entry->mesh.data->primitives.size());
            if (mesh.primitive_first >= primitives)
            {
                continue;
            }
            const std::uint32_t count = std::min(mesh.primitive_count, primitives - mesh.primitive_first);

            const std::vector<material_handle>& materials = registry_.get<components::material>(entity).materials;
            const auto* options = registry_.try_get<components::render_options>(entity);
            if (options == nullptr)
            {
                options = &default_options;
            }

            avis::flags<instance_flags> flags = options->flags;
            if (const auto* shown = registry_.try_get<components::visibility>(entity);
                shown != nullptr && !shown->visible)
            {
                flags.erase(instance_flags::visible);
            }

            const Eigen::Matrix4f world_current =
                graph_.world(registry_.get<components::node_reference>(entity).node).matrix();
            Eigen::Matrix4f world_previous = world_current;
            if (const auto* previous = registry_.try_get<components::previous_world>(entity))
            {
                world_previous = previous->world.matrix();
            }
            else
            {
                flags |= instance_flags::history_invalid; // created this tick: it pops in (§6.3)
            }

            const object_id id = registry_.get<components::identity>(entity).id;
            for (std::uint32_t index = 0; index < count; ++index)
            {
                // The last material repeats when there are fewer than primitives
                const material_handle material = materials.empty() ?
                                                     material_handle{} :
                                                     materials[std::min<std::size_t>(index, materials.size() - 1)];
                target.instances.push_back(
                    {
                        .object_id = id,
                        .mesh = mesh.asset,
                        .submesh = mesh.primitive_first + index,
                        .material = material,
                        .world_previous = world_previous,
                        .world_current = world_current,
                        .layer = options->layer,
                        .layer_priority = options->layer_priority,
                        .flags = flags,
                        .maximum_view_distance = options->maximum_view_distance,
                    });
            }
        }
    }

    void scene::add_lights(snapshot& target)
    {
        for (const auto [entity, reference, light] :
             registry_.view<components::node_reference, components::light>().each())
        {
            target.lights.push_back(
                {
                    .id = id_of(entity),
                    .kind = light.kind,
                    .light_color = light.light_color,
                    .intensity = light.intensity,
                    .range = light.range,
                    .inner_cone_angle = light.inner_cone_angle,
                    .outer_cone_angle = light.outer_cone_angle,
                    .world = graph_.world(reference.node).matrix(),
                    .casts_shadows = light.casts_shadows,
                });
        }
        std::ranges::sort(target.lights, {}, &light_instance::id); // the pool's order changes on destroy
    }

    // ---- signal handlers ----

    template<typename Component>
    void scene::on_owned_write(entt::registry&, entity)
    {
        AVIS_CHECK(
            logger_name,
            owner_writes_ > 0,
            std::format("scene: {} is owned by scene and written only through its members", owned_name<Component>()));
    }

    void scene::on_identity_construct(entt::registry& registry, const entity entity)
    {
        const auto& identity = registry.get<components::identity>(entity);
        ++counters_.entities;
        ++counters_.per_category[static_cast<std::size_t>(identity.category)];
    }

    void scene::on_identity_destroy(entt::registry& registry, const entity entity)
    {
        const auto& identity = registry.get<components::identity>(entity);
        --counters_.entities;
        --counters_.per_category[static_cast<std::size_t>(identity.category)];
    }

    void scene::on_represents_construct(entt::registry&, const entity entity)
    {
        const object_id id = id_of(entity);
        if (id == 0)
        {
            return;
        }

        // Still pending: the GUI hasn't seen the entity yet, so the added change itself becomes hidden
        for (auto change = tree_changes_.rbegin(); change != tree_changes_.rend(); ++change)
        {
            if (change->id == id)
            {
                if (change->kind == tree_change_kind::added)
                {
                    change->hidden = true;
                    return;
                }
                break;
            }
        }
        record_change(tree_change_kind::renamed, entity);
    }

    void scene::on_represents_destroy(entt::registry&, const entity entity)
    {
        if (destroying_ || id_of(entity) == 0)
        {
            return;
        }
        record_change(tree_change_kind::renamed, entity);
        tree_changes_.back().hidden = false; // the component is still there while on_destroy runs
    }

    void scene::on_draw_set_change(entt::registry&, entity)
    {
        draw_order_dirty_ = true;
    }

    void scene::on_mesh_construct(entt::registry& registry, const entity entity)
    {
        const mesh_handle asset = registry.get<components::mesh>(entity).asset;
        mesh_shadow_.emplace(entity, asset);
        retain_valid(assets_, asset);
    }

    void scene::on_mesh_update(entt::registry& registry, const entity entity)
    {
        const mesh_handle asset = registry.get<components::mesh>(entity).asset;
        mesh_handle& retained = mesh_shadow_.get(entity);
        retain_valid(assets_, asset); // before the release, in case it's the same handle
        release_valid(assets_, retained);
        retained = asset;
    }

    void scene::on_mesh_destroy(entt::registry&, const entity entity)
    {
        if (mesh_shadow_.contains(entity))
        {
            release_valid(assets_, mesh_shadow_.get(entity));
            mesh_shadow_.erase(entity);
        }
    }

    void scene::on_material_construct(entt::registry& registry, const entity entity)
    {
        const std::vector<material_handle>& materials = registry.get<components::material>(entity).materials;
        material_shadow_.emplace(entity, materials);
        retain_valid(assets_, materials);
    }

    void scene::on_material_update(entt::registry& registry, const entity entity)
    {
        const std::vector<material_handle>& materials = registry.get<components::material>(entity).materials;
        std::vector<material_handle>& retained = material_shadow_.get(entity);
        std::vector<material_handle> copy = materials; // may throw; nothing changed yet
        retain_valid(assets_, materials);
        release_valid(assets_, retained);
        retained = std::move(copy);
    }

    void scene::on_material_destroy(entt::registry&, const entity entity)
    {
        if (material_shadow_.contains(entity))
        {
            release_valid(assets_, material_shadow_.get(entity));
            material_shadow_.erase(entity);
        }
    }

    void scene::on_label_construct(entt::registry&, entity)
    {
        ++counters_.labels;
    }

    void scene::on_label_destroy(entt::registry&, entity)
    {
        --counters_.labels;
    }

    void scene::on_trail_construct(entt::registry& registry, const entity entity)
    {
        const std::size_t samples = registry.get<components::trail>(entity).samples.size();
        trail_counts_.emplace(entity, samples);
        ++counters_.trails;
        counters_.trail_samples += samples;
    }

    void scene::on_trail_destroy(entt::registry&, const entity entity)
    {
        --counters_.trails;
        if (trail_counts_.contains(entity))
        {
            counters_.trail_samples -= trail_counts_.get(entity);
            trail_counts_.erase(entity);
        }
    }

    void scene::on_entity_destroy(entt::registry&, const entity entity)
    {
        jumped_.remove(entity);
    }
} // namespace avis::scene
