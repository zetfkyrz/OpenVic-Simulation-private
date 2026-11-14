#include "DiplomaticAction.hpp"
#include "openvic-simulation/ai/DiplomacyAI.hpp"

#include <any>
#include <string_view>

#include "openvic-simulation/InstanceManager.hpp"
#include "openvic-simulation/core/object/Date.hpp"
#include "openvic-simulation/core/object/Timespan.hpp"
#include "openvic-simulation/DefinitionManager.hpp"
#include "openvic-simulation/country/CountryInstance.hpp"
#include "openvic-simulation/defines/Define.hpp"
#include "openvic-simulation/defines/DiplomacyDefines.hpp"
#include "openvic-simulation/diplomacy/CountryRelation.hpp"
#include "openvic-simulation/military/War.hpp"
#include "openvic-simulation/types/IdentifierRegistry.hpp"
#include "openvic-simulation/utility/Logger.hpp"

using namespace OpenVic;

DiplomaticActionType::DiplomaticActionType(DiplomaticActionType::Initializer&& initializer)
	: commit_action_caller { std::move(initializer.commit) }, allowed_to_commit { std::move(initializer.allowed) },
	  get_acceptance { std::move(initializer.get_acceptance) } {}

CancelableDiplomaticActionType::CancelableDiplomaticActionType(CancelableDiplomaticActionType::Initializer&& initializer)
	: allowed_to_cancel { std::move(initializer.allowed_cancel) }, DiplomaticActionType { std::move(initializer) } {}

bool DiplomaticActionManager::add_diplomatic_action(
	std::string_view identifier, DiplomaticActionType::Initializer&& initializer
) {
	if (identifier.empty()) {
		spdlog::error_s("Invalid diplomatic action identifier - empty!");
		return false;
	}
	return diplomatic_action_types.emplace_item( identifier, identifier, DiplomaticActionType { std::move(initializer) } );
}

bool DiplomaticActionManager::add_cancelable_diplomatic_action(
	std::string_view identifier, CancelableDiplomaticActionType::Initializer&& initializer
) {
	if (identifier.empty()) {
		spdlog::error_s("Invalid cancelable diplomatic action identifier - empty!");
		return false;
	}
	return diplomatic_action_types.emplace_item( identifier, identifier, CancelableDiplomaticActionType { std::move(initializer) } );
}

DiplomaticActionTickCache DiplomaticActionManager::create_diplomatic_action_tick(
	std::string_view identifier, CountryInstance* sender, CountryInstance* receiver, std::any context_data,
	InstanceManager& instance_manager
) const {
	auto type = diplomatic_action_types.get_item_by_identifier(identifier);

	DiplomaticActionTickCache result { { instance_manager, sender, receiver, context_data }, type };
	type->visit([&](auto type) {
		CountryRelationManager::influence_value_type* influence = nullptr;
		if (type.influence_cost != 0) {
			influence = &result.argument.instance_manager.get_country_relation_manager().assign_or_get_influence_with( //
				result.argument.sender, result.argument.receiver
			);
			if (*influence < type.influence_cost) {
				return;
			}
		}
		if (!(result.allowed_to_commit = type.allowed_to_commit(result.argument))) {
			return;
		}
		result.acceptance = type.get_acceptance(result.argument);
		if (influence != nullptr) {
			*influence -= type.influence_cost;
		}
	});

	return result;
}

bool DiplomaticActionManager::execute_diplomatic_action(
	const std::size_t action_type_index, CountryInstance* const sender, CountryInstance* const receiver,
	std::any context_data, InstanceManager& instance_manager
) const {
	if (OV_unlikely(sender == nullptr || receiver == nullptr || sender == receiver)) {
		spdlog::error_s("Diplomatic action requires two distinct countries!");
		return false;
	}

	DiplomaticActionTypeStorage const* type = get_diplomatic_action_type_by_index(action_type_index);
	if (OV_unlikely(type == nullptr)) {
		spdlog::error_s("Invalid diplomatic action type index: {}", action_type_index);
		return false;
	}

	DiplomaticActionTickCache cache =
		create_diplomatic_action_tick(type->get_identifier(), sender, receiver, std::move(context_data), instance_manager);

	if (!cache.allowed_to_commit) {
		return false;
	}

	// Receiver response policy: an action with an acceptance of its own (call_ally) is refused at 0 or below; the others are always accepted
	// TODO: real AI/human decision making for the other actions (relations, opinions...)
	const bool receiver_accepts = cache.acceptance > 0;
	if (!receiver_accepts) {
		return false;
	}

	// These actions read the third country from the context data, which std::any_cast would throw on if it is missing
	// (exceptions are disabled in the simulation build)
	if (!cache.argument.context_data.has_value()
		&& (type->get_identifier() == "decrease_opinion" || type->get_identifier() == "remove_from_foreign_sphere")) {
		spdlog::error_s("Diplomatic action \"{}\" was sent without the context data it requires!", type->get_identifier());
		return false;
	}

	type->visit([&cache](auto const& action_type) -> void {
		action_type.commit_action(cache.argument);
	});

	return true;
}

bool DiplomaticActionManager::setup_diplomatic_actions(DiplomacyDefines const& defines) {
	using Argument = DiplomaticActionType::Argument;
	// The costs in influence come from defines.lua (the commit functions read the other defines when they run)
	const auto cost = [](fixed_point_t const value) -> CountryRelationManager::influence_value_type {
		return CountryRelationManager::influence_value_type(static_cast<std::uint16_t>(value.floor<int32_t>()));
	};

	bool result = true;

	// TODO: implement  _RELATION_ON_ACCEPT & _RELATION_ON_DECLINE

	result &= add_diplomatic_action(
		"form_alliance",
		{
			.commit =
				[](Argument& arg) {
					arg.instance_manager.get_country_relation_manager().set_country_alliance( //
						arg.sender, arg.receiver, true
					);
				},
			.allowed =
				[](Argument const& arg) {
					return !arg.sender->has_alliance_with(*arg.receiver) && !arg.sender->is_at_war_with(*arg.receiver);
				},
			// An AI country answers by its interests (version 91, docs/DIPLOMACY.md); between players the alliance is made
			.get_acceptance =
				[](Argument const& arg) {
					return arg.receiver->is_ai() ? DiplomacyAI::alliance_acceptance(arg.instance_manager, *arg.sender, *arg.receiver) : 1;
				},
		}
	);
	result &= add_diplomatic_action(
		"end_alliance",
		{
			.commit =
				[](Argument& arg) {
					arg.instance_manager.get_country_relation_manager().set_country_alliance( //
						arg.sender, arg.receiver, false
					);
				},
		}
	);
	// The sender leads the sphere of the receiver of this kind (version 93)
	const auto leads_member_of = [](Argument const& arg, const sphere_kind_t kind) -> bool {
		return arg.sender->get_leads_sphere() && arg.sender->get_sphere_kind() == kind && arg.receiver->get_sphere_owner_untracked() == arg.sender;
	};
	result &= add_diplomatic_action(
		"call_ally",
		{
			// The ally joins the war of the sender (where he leads his side, else his first) on his side (docs/WAR_ASSUMPTIONS.md). The leader of
			// a sphere calls its members too (version 93): a member of its military union must come (an AI that refuses loses prestige), one of
			// its imperial sphere comes when it likes the leader enough
			.commit =
				[leads_member_of](Argument& arg) {
					WarManager& war_manager = arg.instance_manager.get_war_manager();
					War const* const war = war_manager.get_war_to_call_into(*arg.sender);
					if (war == nullptr) {
						return;
					}
					if (leads_member_of(arg, sphere_kind_t::MILITARY) && !arg.sender->has_alliance_with(*arg.receiver)) {
						war_manager.call_union_member(arg.instance_manager, war->id, *arg.sender, *arg.receiver);
						return;
					}
					war_manager.join_war(arg.instance_manager, war->id, *arg.receiver, war->is_attacker(*arg.sender));
				},
			.allowed =
				[leads_member_of](Argument const& arg) {
					WarManager const& war_manager = arg.instance_manager.get_war_manager();
					War const* const war = war_manager.get_war_to_call_into(*arg.sender);
					return war != nullptr
						&& (arg.sender->has_alliance_with(*arg.receiver) || leads_member_of(arg, sphere_kind_t::MILITARY)
							|| leads_member_of(arg, sphere_kind_t::IMPERIAL))
						&& war_manager.can_join_war(*war, war->is_attacker(*arg.sender), *arg.receiver);
				},
			.get_acceptance =
				[leads_member_of](Argument const& arg) {
					WarManager const& war_manager = arg.instance_manager.get_war_manager();
					War const* const war = war_manager.get_war_to_call_into(*arg.sender);
					if (war == nullptr) {
						return 0;
					}
					if (leads_member_of(arg, sphere_kind_t::MILITARY)) {
						return 1; // (the refusal is in the commit: it costs prestige)
					}
					if (war_manager.would_join_war(*war, *arg.receiver) && arg.sender->has_alliance_with(*arg.receiver)) {
						return 1;
					}
					return !arg.receiver->is_ai() || war_manager.imperial_member_helps(arg.instance_manager, *war, *arg.sender, *arg.receiver) ? 1 : 0;
				},
		}
	);
	result &= add_diplomatic_action(
		"request_military_access",
		{
			.commit =
				[](Argument& arg) {
					arg.instance_manager.get_country_relation_manager().set_has_military_access_to( //
						arg.sender, arg.receiver, true
					);
				},
		}
	);
	result &= add_diplomatic_action( //
		"give_military_access",
		{
			.commit =
				[](Argument& arg) {
					arg.instance_manager.get_country_relation_manager().set_has_military_access_to( //
						arg.receiver, arg.sender, true
					);
				},
		}
	);
	// The relations (version 90, docs/DIPLOMACY.md): the button starts (or stops) the process of the sender, every month
	// RELATION_IMPROVE_PER_MONTH up or RELATION_WORSEN_PER_MONTH down for a diplomatic point, until the limit (CountryRelationManager::monthly_update)
	result &= add_diplomatic_action(
		"increase_relations",
		{
			.commit =
				[](Argument& arg) {
					CountryRelationManager& manager = arg.instance_manager.get_country_relation_manager();
					manager.set_relation_process(arg.sender, arg.receiver, manager.get_relation_process(arg.sender, arg.receiver) > 0 ? 0 : 1);
				},
			.allowed =
				[](Argument const& arg) {
					return arg.sender != arg.receiver && !arg.instance_manager.get_country_relation_manager().get_at_war_with(arg.sender, arg.receiver);
				},
		}
	);
	result &= add_diplomatic_action(
		"decrease_relations",
		{
			.commit =
				[](Argument& arg) {
					CountryRelationManager& manager = arg.instance_manager.get_country_relation_manager();
					manager.set_relation_process(arg.sender, arg.receiver, manager.get_relation_process(arg.sender, arg.receiver) < 0 ? 0 : -1);
				},
			.allowed =
				[](Argument const& arg) {
					return arg.sender != arg.receiver && !arg.instance_manager.get_country_relation_manager().get_at_war_with(arg.sender, arg.receiver);
				},
		}
	);
	result &= add_diplomatic_action(
		"war_subsidies",
		{
			.commit =
				[](Argument& arg) {
					arg.instance_manager.get_country_relation_manager().set_war_subsidies_to( //
						arg.sender, arg.receiver, true
					);
				},
		}
	);
	result &= add_diplomatic_action(
		"end_war_subsidies",
		{
			.commit =
				[](Argument& arg) {
					arg.instance_manager.get_country_relation_manager().set_war_subsidies_to( //
						arg.sender, arg.receiver, false
					);
				},
		}
	);
	result &= add_diplomatic_action(
		"declare_war",
		{
			.commit =
				[](Argument& arg) {
					arg.instance_manager.get_war_manager().declare_war_as_country(arg.instance_manager, *arg.sender, *arg.receiver, WarGoal {});
				},
			.allowed =
				[](Argument const& arg) {
					return arg.instance_manager.get_war_manager().can_declare_war(
						*arg.sender, *arg.receiver, arg.instance_manager.get_today()
					);
				},
		}
	);
	result &= add_diplomatic_action(
		"offer_peace",
		{
			// White peace in the first war the two countries fight each other in. Other peace terms are
			// proposed with the propose_peace game action.
			.commit =
				[](Argument& arg) {
					for (War const* war : arg.instance_manager.get_war_manager().get_wars_of(*arg.sender)) {
						if (war->has_participant(*arg.receiver) && war->is_attacker(*arg.sender) != war->is_attacker(*arg.receiver)) {
							arg.instance_manager.get_war_manager().propose_peace(
								arg.instance_manager, war->id, *arg.sender, WarManager::peace_type_t::WHITE_PEACE
							);
							break;
						}
					}
				},
			.allowed =
				[](Argument const& arg) {
					return arg.sender->is_at_war_with(*arg.receiver);
				},
		}
	);
	result &= add_diplomatic_action(
		"command_units",
		{
			.commit =
				[](Argument& arg) {
					arg.instance_manager.get_country_relation_manager().assign_or_get_commands_units( //
						arg.sender, arg.receiver
					) = true;
				},
		}
	);
	result &= add_diplomatic_action(
		"discredit",
		{
			.influence_cost = cost(defines.get_discredit_influence_cost()),
			.commit =
				[](Argument& arg) {
					arg.instance_manager.get_country_relation_manager().set_discredited_date( //
						arg.sender, arg.receiver,
						arg.instance_manager.get_today()
							+ arg.instance_manager.definition_manager.get_define_manager().get_diplomacy_defines().get_discredit_days()
					);
				},
		}
	);
	result &= add_diplomatic_action(
		"expel_advisors",
		{
			.influence_cost = cost(defines.get_expeladvisors_influence_cost()),
			.commit = [](Argument& arg) {},
		}
	);
	result &= add_diplomatic_action(
		"ban_embassy",
		{
			.influence_cost = cost(defines.get_banembassy_influence_cost()),
			.commit =
				[](Argument& arg) {
					arg.instance_manager.get_country_relation_manager().set_embassy_banned_date( //
						arg.sender, arg.receiver,
						arg.instance_manager.get_today()
							+ arg.instance_manager.definition_manager.get_define_manager().get_diplomacy_defines().get_banembassy_days()
					);
				},
		}
	);
	result &= add_diplomatic_action(
		"increase_opinion",
		{
			.influence_cost = cost(defines.get_increaseopinion_influence_cost()),
			.commit =
				[](Argument& arg) {
					arg.instance_manager.get_country_relation_manager().assign_or_get_country_opinion( //
						arg.sender, arg.receiver
					)++;
				},
			.allowed =
				[](Argument const& arg) {
					return true;
				},
		}
	);
	result &= add_diplomatic_action(
		"decrease_opinion",
		{
			.influence_cost = cost(defines.get_decreaseopinion_influence_cost()),
			.commit =
				[](Argument& arg) {
					--arg.instance_manager.get_country_relation_manager().assign_or_get_country_opinion( //
						std::any_cast<CountryInstance*>(arg.context_data), arg.receiver
					);
				},
			.allowed =
				[](Argument const& arg) {
					return true;
				},
		}
	);
	result &= add_diplomatic_action(
		"add_to_sphere",
		{
			.influence_cost = cost(defines.get_addtosphere_influence_cost()),
			.commit =
				[](Argument& arg) {
					arg.instance_manager.get_country_relation_manager().assign_or_get_country_opinion( //
						arg.sender, arg.receiver
					) = CountryRelationManager::OpinionType::Sphere;
				},
			.allowed =
				[](Argument const& arg) {
					return true;
				},
		}
	);
	result &= add_diplomatic_action(
		"remove_from_foreign_sphere",
		{
			.influence_cost = cost(defines.get_removefromsphere_influence_cost()),
			.commit =
				[](Argument& arg) {
					--arg.instance_manager.get_country_relation_manager().assign_or_get_country_opinion( //
						std::any_cast<CountryInstance*>(arg.context_data), arg.receiver
					);
					arg.instance_manager.get_country_relation_manager().assign_or_get_country_relation( //
						arg.sender, arg.receiver
					) -= 10;
				},
			.allowed =
				[](Argument const& arg) {
					return true;
				},
		}
	);
	result &= add_diplomatic_action(
		"remove_from_own_sphere",
		{
			.influence_cost = cost(defines.get_removefromsphere_influence_cost()),
			.commit =
				[](Argument& arg) {
					--arg.instance_manager.get_country_relation_manager().assign_or_get_country_opinion( //
						arg.sender, arg.receiver
					);
					arg.instance_manager.get_country_relation_manager().assign_or_get_country_relation( //
						arg.sender, arg.receiver
					) -= 10;
					// TODO: implement REMOVEFROMSPHERE_PRESTIGE_COST and REMOVEFROMSPHERE_INFAMY_COST
				},
			.allowed =
				[](Argument const& arg) {
					return true;
				},
		}
	);
	result &= add_diplomatic_action(
		"justify_war",
		{
			// JUSTIFY_WAR_DAYS days, then a casus belli against the receiver (docs/WAR_ASSUMPTIONS.md, "Обоснование войны")
			.commit =
				[](Argument& arg) {
					arg.instance_manager.get_war_manager().start_justification(*arg.sender, *arg.receiver, arg.instance_manager.get_today());
				},
			.allowed =
				[](Argument const& arg) {
					return arg.instance_manager.get_war_manager()
						.get_justify_block_reason(*arg.sender, *arg.receiver, arg.instance_manager.get_today()).empty();
				},
		}
	);
	result &= add_diplomatic_action(
		"give_vision",
		{
			.commit =
				[](Argument& arg) {
					arg.instance_manager.get_country_relation_manager().set_has_vision( //
						arg.sender, arg.receiver, true
					);
				},
		}
	);
	result &= add_diplomatic_action(
		"remove_vision",
		{
			.commit =
				[](Argument& arg) {
					arg.instance_manager.get_country_relation_manager().set_has_vision( //
						arg.sender, arg.receiver, false
					);
				},
		}
	);
	diplomatic_action_types.lock();

	return result;
}
