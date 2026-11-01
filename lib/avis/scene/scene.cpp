#include "avis/scene/scene.h"

#include "avis/detail/check.h"
#include "avis/scene/components/viewing.h"
#include "avis/scene/error.h"

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
        next_helper_id_{ first_helper_id }
    {
        using namespace components;

        registry_.on_construct<identity>().connect<&scene::on_identity_construct>(*this);
        registry_.on_destroy<identity>().connect<&scene::on_identity_destroy>(*this);
        registry_.on_construct<represents>().connect<&scene::on_represents_construct>(*this);
        registry_.on_destroy<represents>().connect<&scene::on_represents_destroy>(*this);
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

    void scene::on_trail_construct(entt::registry&, entity)
    {
        ++counters_.trails;
    }

    void scene::on_trail_destroy(entt::registry&, entity)
    {
        --counters_.trails;
    }
} // namespace avis::scene
