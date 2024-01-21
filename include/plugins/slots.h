// Copyright (c) 2024 UltiMaker
// CuraEngine is released under the terms of the AGPLv3 or higher

#ifndef PLUGINS_SLOTS_H
#define PLUGINS_SLOTS_H

#ifdef ENABLE_PLUGINS
#include <exception>
#include <memory>
#include <tuple>
#include <utility>

#include "WallToolPaths.h"
#include "cura/plugins/slots/broadcast/v0/broadcast.grpc.pb.h"
#include "cura/plugins/slots/gcode_paths/v0/modify.grpc.pb.h"
#include "cura/plugins/slots/infill/v0/generate.grpc.pb.h"
#include "cura/plugins/slots/postprocess/v0/modify.grpc.pb.h"
#include "cura/plugins/slots/simplify/v0/modify.grpc.pb.h"
#include "cura/plugins/v0/slot_id.pb.h"
#include "infill.h"
#include "plugins/converters.h"
#include "plugins/slotproxy.h"
#include "plugins/types.h"
#include "plugins/validator.h"
#include "utils/Point2LL.h"
#include "utils/Simplify.h" // TODO: Remove once the simplify slot has been removed
#include "utils/polygon.h"
#include "utils/types/char_range_literal.h"

namespace cura
{
namespace plugins
{
namespace details
{
struct default_process
{
    constexpr auto operator()(auto&& arg, auto&&...)
    {
        return std::forward<decltype(arg)>(arg);
    };
};

struct simplify_default
{
    auto operator()(auto&& arg, auto&&... args)
    {
        const Simplify simplify{ std::forward<decltype(args)>(args)... };
        return simplify.polygon(std::forward<decltype(arg)>(arg));
    }
};

struct infill_generate_default
{
    std::tuple<std::vector<VariableWidthLines>, Polygons, Polygons> operator()([[maybe_unused]] auto&&... args)
    {
        // this code is only reachable when no slot is registered while the infill type is requested to be
        // generated by a plugin; this should not be possible to set up in the first place. Return an empty
        // infill.
        return {};
    }
};

/**
 * @brief Alias for the Simplify slot.
 *
 * This alias represents the Simplify slot, which is used for simplifying polygons.
 *
 * @tparam Default The default behavior when no plugin is registered.
 */
template<class Default = default_process>
using slot_simplify_
    = SlotProxy<v0::SlotID::SIMPLIFY_MODIFY, "0.1.0-alpha", slots::simplify::v0::modify::SimplifyModifyService::Stub, Validator, simplify_request, simplify_response, Default>;

template<class Default = default_process>
using slot_infill_generate_ = SlotProxy<
    v0::SlotID::INFILL_GENERATE,
    "0.1.0-alpha",
    slots::infill::v0::generate::InfillGenerateService::Stub,
    Validator,
    infill_generate_request,
    infill_generate_response,
    Default>;

/**
 * @brief Alias for the Postprocess slot.
 *
 * This alias represents the Postprocess slot, which is used for post-processing G-code.
 *
 * @tparam Default The default behavior when no plugin is registered.
 */
template<class Default = default_process>
using slot_postprocess_ = SlotProxy<
    v0::SlotID::POSTPROCESS_MODIFY,
    "0.1.0-alpha",
    slots::postprocess::v0::modify::PostprocessModifyService::Stub,
    Validator,
    postprocess_request,
    postprocess_response,
    Default>;

template<class Default = default_process>
using slot_settings_broadcast_
    = SlotProxy<v0::SlotID::SETTINGS_BROADCAST, "0.1.0-alpha", slots::broadcast::v0::BroadcastService::Stub, Validator, broadcast_settings_request, empty, Default>;

template<class Default = default_process>
using slot_gcode_paths_modify_ = SlotProxy<
    v0::SlotID::GCODE_PATHS_MODIFY,
    "0.1.0-alpha",
    slots::gcode_paths::v0::modify::GCodePathsModifyService::Stub,
    Validator,
    gcode_paths_modify_request,
    gcode_paths_modify_response,
    Default>;

template<typename... Types>
struct Typelist
{
};

template<typename TList, template<typename> class Unit>
class Registry;

template<template<typename> class Unit>
class Registry<Typelist<>, Unit>
{
public:
    constexpr void connect([[maybe_unused]] auto&&... args) noexcept
    {
    }

    template<v0::SlotID S>
    constexpr void broadcast([[maybe_unused]] auto&&... args) noexcept
    {
    } // Base case, do nothing
};

template<typename T, typename... Types, template<typename> class Unit>
class Registry<Typelist<T, Types...>, Unit> : public Registry<Typelist<Types...>, Unit>
{
public:
    using ValueType = T;
    using Base = Registry<Typelist<Types...>, Unit>;
    using Base::broadcast;
    friend Base;

    template<v0::SlotID S>
    constexpr auto& get()
    {
        return get_type<S>().proxy;
    }

    template<v0::SlotID S>
    constexpr auto modify(auto& original_value, auto&&... args)
    {
        return get<S>().modify(original_value, std::forward<decltype(args)>(args)...);
    }

    template<v0::SlotID S>
    constexpr auto generate(auto&&... args)
    {
        return get<S>().generate(std::forward<decltype(args)>(args)...);
    }

    void connect(const v0::SlotID& slot_id, auto name, auto& version, auto&& channel)
    {
        if (slot_id == T::slot_id)
        {
            using Tp = typename Unit<T>::value_type;
            value_.proxy = Tp{ name, version, std::forward<decltype(channel)>(channel) };
            return;
        }
        Base::connect(slot_id, name, version, std::forward<decltype(channel)>(channel));
    }

    template<v0::SlotID S>
    void broadcast(auto&&... args)
    {
        value_.proxy.template broadcast<S>(std::forward<decltype(args)>(args)...);
        Base::template broadcast<S>(std::forward<decltype(args)>(args)...);
    }

protected:
    template<v0::SlotID S>
    constexpr auto& get_type()
    {
        return get_helper<S>(std::bool_constant<S == ValueType::slot_id>{});
    }

    template<v0::SlotID S>
    constexpr auto& get_helper(std::true_type)
    {
        return value_;
    }

    template<v0::SlotID S>
    constexpr auto& get_helper(std::false_type)
    {
        return Base::template get_type<S>();
    }

    Unit<ValueType> value_;
};

template<typename TList, template<typename> class Unit>
class SingletonRegistry
{
public:
    static Registry<TList, Unit>& instance()
    {
        static Registry<TList, Unit> instance;
        return instance;
    }

private:
    constexpr SingletonRegistry() = default;
};

template<typename T>
struct Holder
{
    using value_type = T;
    T proxy;
};

} // namespace details

using slot_gcode_paths_modify = details::slot_gcode_paths_modify_<>;
using slot_infill_generate = details::slot_infill_generate_<details::infill_generate_default>;
using slot_postprocess = details::slot_postprocess_<>;
using slot_settings_broadcast = details::slot_settings_broadcast_<>;
using slot_simplify = details::slot_simplify_<details::simplify_default>;

using SlotTypes = details::Typelist<slot_gcode_paths_modify, slot_infill_generate, slot_postprocess, slot_settings_broadcast, slot_simplify>;

} // namespace plugins
using slots = plugins::details::SingletonRegistry<plugins::SlotTypes, plugins::details::Holder>;

} // namespace cura

#else // No Engine plugin support
namespace cura
{

namespace plugins::v0
{
// FIXME: use same SlotID as defined in Cura.proto
enum SlotID : int
{
    SETTINGS_BROADCAST = 0,
    SIMPLIFY_MODIFY = 100,
    POSTPROCESS_MODIFY = 101,
    INFILL_MODIFY = 102,
    GCODE_PATHS_MODIFY = 103,
    INFILL_GENERATE = 200,
    DIALECT_GENERATE = 201,
    SlotID_INT_MIN_SENTINEL_DO_NOT_USE_ = std::numeric_limits<int32_t>::min(),
    SlotID_INT_MAX_SENTINEL_DO_NOT_USE_ = std::numeric_limits<int32_t>::max()
};
} // namespace plugins::v0

namespace slots
{
namespace details
{
struct Slots
{
    template<plugins::v0::SlotID S>
    constexpr auto modify(auto&& data, auto&&... args) noexcept
    {
        return std::forward<decltype(data)>(data);
    }

    template<plugins::v0::SlotID S>
    constexpr auto broadcast(auto&&... args) noexcept
    {
    }

    constexpr auto connect(auto&&... args) noexcept
    {
    }
};
} // namespace details

constexpr details::Slots instance() noexcept
{
    return {};
}
} // namespace slots

} // namespace cura


#endif // ENABLE_PLUGINS

#endif // PLUGINS_SLOTS_H
