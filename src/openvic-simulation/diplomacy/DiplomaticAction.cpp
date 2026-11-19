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
#include "openvic-simulation/diplomacy/Subjects.hpp"
#include "openvic-simulation/diplomacy/Unions.hpp"
#include "openvic-simulation/military/War.hpp"
#include "openvic-simulation/misc/NotificationLog.hpp"
#include "openvic-simulation/map/MapInstance.hpp"
#include "openvic-simulation/map/ProvinceInstance.hpp"
#include "openvic-simulation/military/UnitInstanceGroup.hpp"
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

	// These actions read the third country from the context data, which std::any_cast would throw on if it is missing
	// (exceptions are disabled in the simulation build)
	if (!cache.argument.context_data.has_value()
		&& (type->get_identifier() == "decrease_opinion" || type->get_identifier() == "remove_from_foreign_sphere")) {
		spdlog::error_s("Diplomatic action \"{}\" was sent without the context data it requires!", type->get_identifier());
		return false;
	}

	// The price in diplomatic points (version 96, stage D1 of docs/PLAN_DIPLOMACY_VIC2.md): paid when the action is sent, accepted or not
	const action_terms_t terms = get_terms(action_type_index);
	if (sender->get_diplomatic_points() < terms.cost) {
		return false;
	}
	CountryRelationManager& relations = instance_manager.get_country_relation_manager();
	const Date today = instance_manager.get_today();

	// The country of a player answers itself: the action waits for its answer (a proposal)
	if (terms.needs_consent && !receiver->is_ai()) {
		if (relations.has_proposal(action_type_index, sender, receiver)) {
			return false;
		}
		sender->set_diplomatic_points(sender->get_diplomatic_points() - terms.cost);
		relations.add_proposal(action_type_index, sender, receiver, today);
		instance_manager.get_notification_log().add(today, receiver, "diplomatic_proposal", type->get_identifier(), sender);
		return true;
	}

	sender->set_diplomatic_points(sender->get_diplomatic_points() - terms.cost);
	// An AI answers by its acceptance (the actions without one are always accepted)
	if (cache.acceptance <= 0) {
		relations.change_relation(sender, receiver, terms.on_decline);
		if (terms.dishonour && defines != nullptr) {
			receiver->add_prestige(defines->get_dishonored_callally_prestige_penalty());
		}
		instance_manager.get_notification_log().add(today, sender, "diplomatic_declined", type->get_identifier(), receiver);
		return false;
	}

	type->visit([&cache](auto const& action_type) -> void {
		action_type.commit_action(cache.argument);
	});
	relations.change_relation(sender, receiver, terms.on_accept);

	return true;
}

DiplomaticActionManager::action_terms_t DiplomaticActionManager::get_terms(const std::string_view identifier) const {
	if (defines == nullptr) {
		return {};
	}
	DiplomacyDefines const& d = *defines;
	const auto value = [](fixed_point_t const number) -> std::int32_t {
		return number.round<std::int32_t>();
	};
	if (identifier == "form_alliance") {
		return { d.get_alliance_diplomatic_cost(), value(d.get_alliance_relation_on_accept()), value(d.get_alliance_relation_on_decline()), true };
	}
	if (identifier == "end_alliance") {
		return { d.get_cancelalliance_diplomatic_cost(), value(d.get_cancelalliance_relation_on_accept()), 0, false };
	}
	if (identifier == "call_ally") {
		// the call to arms (version 97): the country of a player answers it, a refusal is a dishonour
		return { d.get_callally_diplomatic_cost(), value(d.get_callally_relation_on_accept()), value(d.get_callally_relation_on_decline()), true, true };
	}
	if (identifier == "cancel_military_access") {
		return { d.get_cancelaskmilaccess_diplomatic_cost(), value(d.get_cancelaskmilaccess_relation_on_accept()), 0, false };
	}
	if (identifier == "revoke_military_access") {
		return { d.get_cancelgivemilaccess_diplomatic_cost(), value(d.get_cancelgivemilaccess_relation_on_accept()), 0, false };
	}
	if (identifier == "request_military_access") {
		return { d.get_askmilaccess_diplomatic_cost(), value(d.get_askmilaccess_relation_on_accept()),
			value(d.get_askmilaccess_relation_on_decline()), true };
	}
	if (identifier == "give_military_access") {
		return { d.get_givemilaccess_diplomatic_cost(), value(d.get_givemilaccess_relation_on_accept()),
			value(d.get_givemilaccess_relation_on_decline()), true };
	}
	if (identifier == "war_subsidies") {
		return { d.get_warsubsidy_diplomatic_cost(), value(d.get_warsubsidy_relation_on_accept()), 0, true };
	}
	if (identifier == "end_war_subsidies") {
		return { d.get_cancelwarsubsidy_diplomatic_cost(), value(d.get_cancelwarsubsidy_relation_on_accept()), 0, false };
	}
	// the actions of an overlord (version 101): the annexation needs the consent of the subject
	if (OpenVicDiplomacyDefines const* const o = CountryRelationManager::diplomacy_defines; o != nullptr) {
		if (identifier == "annex_subject") {
			return { o->subject_annex_cost, 0, o->subject_annex_decline_relation, true };
		}
		if (identifier == "release_subject") {
			return { fixed_point_t::_0, o->subject_release_relation, 0, false };
		}
		if (identifier == "raise_subject_tribute") {
			return { o->subject_tribute_change_cost, o->subject_raise_tribute_relation, 0, false };
		}
		if (identifier == "lower_subject_tribute") {
			return { fixed_point_t::_0, o->subject_lower_tribute_relation, 0, false };
		}
		if (identifier == "grant_autonomy") {
			return { o->subject_autonomy_cost, o->subject_autonomy_relation, 0, false };
		}
		if (identifier == "subject_join_war") {
			return { o->subject_coerce_cost, o->subject_coerce_relation, 0, false };
		}
		// the invitation into a union (version 103): the country answers it
		if (identifier == "join_union") {
			return { o->union_invite_cost, 0, o->union_invite_decline_relation, true };
		}
	}
	return {};
}

DiplomaticActionManager::action_terms_t DiplomaticActionManager::get_terms(const std::size_t action_type_index) const {
	DiplomaticActionTypeStorage const* type = get_diplomatic_action_type_by_index(action_type_index);
	return type != nullptr ? get_terms(type->get_identifier()) : action_terms_t {};
}

std::optional<std::size_t> DiplomaticActionManager::get_action_index(const std::string_view identifier) const {
	std::size_t index = 0;
	for (DiplomaticActionTypeStorage const& type : get_diplomatic_action_types()) {
		if (type.get_identifier() == identifier) {
			return index;
		}
		index++;
	}
	return std::nullopt;
}

bool DiplomaticActionManager::propose(
	const std::size_t action_type_index, CountryInstance* const sender, CountryInstance* const receiver, InstanceManager& instance_manager,
	const bool respect_cooldown
) const {
	DiplomaticActionTypeStorage const* type = get_diplomatic_action_type_by_index(action_type_index);
	if (type == nullptr || sender == nullptr || receiver == nullptr || sender == receiver) {
		return false;
	}
	CountryRelationManager& relations = instance_manager.get_country_relation_manager();
	const Date today = instance_manager.get_today();
	if (relations.has_proposal(action_type_index, sender, receiver) || (respect_cooldown && today < relations.get_proposal_cooldown(sender, receiver))) {
		return false;
	}
	if (!create_diplomatic_action_tick(type->get_identifier(), sender, receiver, {}, instance_manager).allowed_to_commit) {
		return false;
	}
	relations.add_proposal(action_type_index, sender, receiver, today);
	instance_manager.get_notification_log().add(today, receiver, "diplomatic_proposal", type->get_identifier(), sender);
	return true;
}

void DiplomaticActionManager::finish_proposal(
	const CountryRelationManager::proposal_t proposal, const bool accept, InstanceManager& instance_manager
) const {
	CountryRelationManager& relations = instance_manager.get_country_relation_manager();
	relations.remove_proposal(proposal.id);
	DiplomaticActionTypeStorage const* type = get_diplomatic_action_type_by_index(proposal.action_index);
	if (type == nullptr) {
		return;
	}
	const Date today = instance_manager.get_today();
	const action_terms_t terms = get_terms(proposal.action_index);
	// The action is done if it still can be (the countries may have gone to war in the meantime)
	DiplomaticActionTickCache cache =
		create_diplomatic_action_tick(type->get_identifier(), proposal.sender, proposal.receiver, {}, instance_manager);
	// (a proposal that cannot be done any more, the war of a call to arms is over: it just goes, nobody is dishonoured)
	if (!cache.allowed_to_commit) {
		return;
	}
	if (accept) {
		type->visit([&cache](auto const& action_type) -> void {
			action_type.commit_action(cache.argument);
		});
		relations.change_relation(proposal.sender, proposal.receiver, terms.on_accept);
		instance_manager.get_notification_log().add(today, proposal.sender, "diplomatic_accepted", type->get_identifier(), proposal.receiver);
		return;
	}
	relations.change_relation(proposal.sender, proposal.receiver, terms.on_decline);
	if (terms.dishonour && defines != nullptr) {
		proposal.receiver->add_prestige(defines->get_dishonored_callally_prestige_penalty());
	}
	if (type->get_identifier() != "call_ally" && OpenVic::CountryRelationManager::diplomacy_defines != nullptr) {
		relations.set_proposal_cooldown(
			proposal.sender, proposal.receiver,
			today + Timespan::from_months(CountryRelationManager::diplomacy_defines->diplomatic_proposal_cooldown_months)
		);
	}
	instance_manager.get_notification_log().add(today, proposal.sender, "diplomatic_declined", type->get_identifier(), proposal.receiver);
}

bool DiplomaticActionManager::answer_proposal(
	const std::uint32_t id, CountryInstance const& receiver, const bool accept, InstanceManager& instance_manager
) const {
	CountryRelationManager::proposal_t const* proposal = instance_manager.get_country_relation_manager().find_proposal(id);
	if (proposal == nullptr || proposal->receiver != &receiver) {
		return false;
	}
	finish_proposal(*proposal, accept, instance_manager);
	return true;
}

void DiplomaticActionManager::call_player_allies(InstanceManager& instance_manager, CountryInstance& leader) const {
	const std::optional<std::size_t> index = get_action_index("call_ally");
	if (!index.has_value()) {
		return;
	}
	for (CountryInstance& country : instance_manager.get_country_instance_manager().get_country_instances()) {
		if (&country != &leader && !country.is_ai() && country.exists() && leader.has_alliance_with(country)) {
			propose(*index, &leader, &country, instance_manager, false);
		}
	}
}

void DiplomaticActionManager::daily_update(InstanceManager& instance_manager) const {
	CountryRelationManager& relations = instance_manager.get_country_relation_manager();
	CountryInstanceManager& countries = instance_manager.get_country_instance_manager();
	// The war subsidies (version 97): WARSUBSIDIES_PERCENT of the military spending of the receiver of the day, as much as the sender has;
	// they end when the receiver is at peace. (a copy: ending changes the map; in the order of the map)
	if (defines != nullptr && !relations.get_war_subsidies_to_values().empty()) {
		memory::vector<std::pair<CountryInstance*, CountryInstance*>> paying;
		for (auto const& [pair, active] : relations.get_war_subsidies_to_values()) {
			if (active) {
				paying.emplace_back(
					&countries.get_country_instance_by_index(pair.first->index), &countries.get_country_instance_by_index(pair.second->index)
				);
			}
		}
		for (auto const& [sender, receiver] : paying) {
			if (!receiver->is_at_war() || sender->is_at_war_with(*receiver)) {
				relations.set_war_subsidies_to(sender, receiver, false);
				instance_manager.get_notification_log().add(instance_manager.get_today(), sender, "war_subsidies_ended", "", receiver);
				continue;
			}
			// (the size the sender chose: 50 to 150 % of the subsidy of Vic2, version 99)
			const fixed_point_t amount = std::min(
				defines->get_warsubsidies_percent() * receiver->get_actual_military_spending().load() * relations.get_war_subsidy_level(sender, receiver) / 100,
				sender->get_cash_stockpile().load()
			);
			if (amount > fixed_point_t::_0) {
				sender->spend_cash(amount);
				receiver->add_cash(amount);
			}
		}
	}
	if (relations.get_proposals().empty() || CountryRelationManager::diplomacy_defines == nullptr) {
		return;
	}
	const Date today = instance_manager.get_today();
	const std::int32_t days = CountryRelationManager::diplomacy_defines->diplomatic_proposal_days;
	// (a copy: declining removes the proposal; the oldest first, in the order they were made)
	const memory::vector<CountryRelationManager::proposal_t> waiting = relations.get_proposals();
	for (CountryRelationManager::proposal_t const& proposal : waiting) {
		if ((today - proposal.date).to_int() >= days) {
			finish_proposal(proposal, false, instance_manager);
		}
	}
}

bool DiplomaticActionManager::setup_diplomatic_actions(DiplomacyDefines const& defines) {
	using Argument = DiplomaticActionType::Argument;
	// The costs in influence come from defines.lua (the commit functions read the other defines when they run)
	const auto cost = [](fixed_point_t const value) -> CountryRelationManager::influence_value_type {
		return CountryRelationManager::influence_value_type(static_cast<std::uint16_t>(value.floor<int32_t>()));
	};

	bool result = true;
	this->defines = &defines;
	// (the costs in diplomatic points and the changes of the relations when an action is accepted or declined: get_terms, version 96)

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
					// (a puppet has no foreign policy, version 101)
					return !arg.sender->has_alliance_with(*arg.receiver) && !arg.sender->is_at_war_with(*arg.receiver) && !arg.sender->is_puppet()
						&& !arg.receiver->is_puppet();
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
			// An AI gives the access by its relation (version 96, DiplomacyAI::military_access_acceptance)
			.get_acceptance =
				[](Argument const& arg) {
					return DiplomacyAI::military_access_acceptance(arg.instance_manager, *arg.sender, *arg.receiver);
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
			// An AI takes the access that is offered unless it is at war with the sender
			.get_acceptance =
				[](Argument const& arg) {
					return arg.receiver->is_at_war_with(*arg.sender) ? 0 : 1;
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
			// (only to a country at war that we do not fight, version 97: the money goes every day, DiplomaticActionManager::daily_update)
			.allowed =
				[](Argument const& arg) {
					return arg.receiver->is_at_war() && !arg.sender->is_at_war_with(*arg.receiver)
						&& !arg.sender->is_sending_war_subsidy_to(*arg.receiver);
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
	// Version 97 (stage D2): give up our access through the receiver; take back the access of the receiver through our land, its armies
	// there go to its nearest province where they may stand
	result &= add_diplomatic_action(
		"cancel_military_access",
		{
			.commit =
				[](Argument& arg) {
					arg.instance_manager.get_country_relation_manager().set_has_military_access_to(arg.sender, arg.receiver, false);
				},
			.allowed =
				[](Argument const& arg) {
					return arg.sender->has_military_access_to(*arg.receiver);
				},
		}
	);
	result &= add_diplomatic_action(
		"revoke_military_access",
		{
			.commit =
				[](Argument& arg) {
					arg.instance_manager.get_country_relation_manager().set_has_military_access_to(arg.receiver, arg.sender, false);
					expel_armies(arg.instance_manager, *arg.receiver, *arg.sender);
				},
			.allowed =
				[](Argument const& arg) {
					return arg.receiver->has_military_access_to(*arg.sender);
				},
		}
	);
	// Version 101 (stage D6, diplomacy/Subjects.hpp): the actions of an overlord with its subject
	const auto our_subject = [](Argument const& arg) -> bool {
		return arg.receiver->overlord == arg.sender;
	};
	result &= add_diplomatic_action(
		"annex_subject",
		{
			.commit =
				[](Argument& arg) {
					Subjects::annex(arg.instance_manager, *arg.sender, *arg.receiver);
				},
			.allowed =
				[](Argument const& arg) {
					return Subjects::get_annex_block_reason(arg.instance_manager, *arg.sender, *arg.receiver).empty();
				},
		}
	);
	result &= add_diplomatic_action(
		"release_subject",
		{
			.commit =
				[](Argument& arg) {
					Subjects::release(arg.instance_manager, *arg.receiver);
				},
			.allowed = our_subject,
		}
	);
	result &= add_diplomatic_action(
		"raise_subject_tribute",
		{
			.commit =
				[](Argument& arg) {
					OpenVicDiplomacyDefines const& o = *CountryRelationManager::diplomacy_defines;
					arg.receiver->subject_tribute_pct = std::min(arg.receiver->subject_tribute_pct + o.subject_tribute_step_pct,
						Subjects::get_max_tribute_pct(o, arg.receiver->subject_type));
				},
			.allowed =
				[our_subject](Argument const& arg) {
					return our_subject(arg) && CountryRelationManager::diplomacy_defines != nullptr && Subjects::tribute_is_changeable(arg.receiver->subject_type)
						&& arg.receiver->subject_tribute_pct < Subjects::get_max_tribute_pct(*CountryRelationManager::diplomacy_defines, arg.receiver->subject_type);
				},
		}
	);
	result &= add_diplomatic_action(
		"lower_subject_tribute",
		{
			.commit =
				[](Argument& arg) {
					OpenVicDiplomacyDefines const& o = *CountryRelationManager::diplomacy_defines;
					arg.receiver->subject_tribute_pct = std::max(arg.receiver->subject_tribute_pct - o.subject_tribute_step_pct,
						Subjects::get_min_tribute_pct(o, arg.receiver->subject_type));
				},
			.allowed =
				[our_subject](Argument const& arg) {
					return our_subject(arg) && CountryRelationManager::diplomacy_defines != nullptr && Subjects::tribute_is_changeable(arg.receiver->subject_type)
						&& arg.receiver->subject_tribute_pct > Subjects::get_min_tribute_pct(*CountryRelationManager::diplomacy_defines, arg.receiver->subject_type);
				},
		}
	);
	result &= add_diplomatic_action(
		"grant_autonomy",
		{
			// the next kind of the table; a dominion is set free
			.commit =
				[](Argument& arg) {
					const CountryInstance::subject_type_t next = Subjects::get_more_autonomy(arg.receiver->subject_type);
					if (next == CountryInstance::subject_type_t::NONE) {
						Subjects::release(arg.instance_manager, *arg.receiver);
					} else {
						Subjects::set_type(arg.instance_manager, *arg.receiver, next);
						arg.instance_manager.get_notification_log().add(
							arg.instance_manager.get_today(), arg.receiver, "became_subject", Subjects::get_type_key(next), arg.sender
						);
					}
				},
			.allowed = our_subject,
		}
	);
	result &= add_diplomatic_action(
		"subject_join_war",
		{
			// the coercion: a colonial subject or a crown land joins the war of its overlord (not a puppet, not a protectorate)
			.commit =
				[](Argument& arg) {
					WarManager& war_manager = arg.instance_manager.get_war_manager();
					if (War const* const war = war_manager.get_war_to_call_into(*arg.sender); war != nullptr) {
						war_manager.join_war(arg.instance_manager, war->id, *arg.receiver, war->is_attacker(*arg.sender));
					}
				},
			.allowed =
				[our_subject](Argument const& arg) {
					if (!our_subject(arg) || !Subjects::tribute_is_changeable(arg.receiver->subject_type)) {
						return false;
					}
					WarManager const& war_manager = arg.instance_manager.get_war_manager();
					War const* const war = war_manager.get_war_to_call_into(*arg.sender);
					return war != nullptr && war_manager.can_join_war(*war, war->is_attacker(*arg.sender), *arg.receiver);
				},
		}
	);
	// Version 103 (stage D8, diplomacy/Unions.hpp): the union invites a small country of its culture group in its sphere or a subject
	result &= add_diplomatic_action(
		"join_union",
		{
			.commit =
				[](Argument& arg) {
					Unions::join_union(arg.instance_manager, *arg.sender, *arg.receiver);
				},
			.allowed =
				[](Argument const& arg) {
					return Unions::may_join(arg.instance_manager, *arg.sender, *arg.receiver);
				},
			.get_acceptance =
				[](Argument const& arg) {
					OpenVicDiplomacyDefines const* const o = CountryRelationManager::diplomacy_defines;
					return o != nullptr && arg.instance_manager.get_country_relation_manager().get_effective_relation(arg.receiver, arg.sender)
						>= o->union_join_relation ? 1 : 0;
				},
		}
	);
	diplomatic_action_types.lock();

	return result;
}

void DiplomaticActionManager::expel_armies(InstanceManager& instance_manager, CountryInstance& country, CountryInstance const& from) {
	MapInstance& map = instance_manager.get_map_instance();
	for (ArmyInstance& army : country.get_armies()) {
		ProvinceInstance& location = army.get_location();
		if (location.get_owner() != &from || army.is_in_combat() || UnitInstanceGroup::can_country_enter(country, location)) {
			continue;
		}
		// the nearest province (by steps over the land) where the army may stand
		memory::vector<ProvinceInstance*> frontier { &location };
		memory::vector<ProvinceInstance*> seen { &location };
		ProvinceInstance* destination = nullptr;
		for (std::size_t i = 0; i < frontier.size() && destination == nullptr && i < 4096; ++i) {
			for (ProvinceDefinition::adjacency_t const& adjacency : frontier[i]->province_definition.get_adjacencies()) {
				if (adjacency.get_type() == ProvinceDefinition::adjacency_t::type_t::IMPASSABLE || static_cast<ProvinceDefinition const&>(adjacency.get_to()).is_water()) {
					continue;
				}
				ProvinceInstance& next = map.get_province_instance_by_definition(adjacency.get_to());
				if (std::find(seen.begin(), seen.end(), &next) != seen.end()) {
					continue;
				}
				seen.push_back(&next);
				if (UnitInstanceGroup::can_country_enter(country, next)) {
					destination = &next;
					break;
				}
				frontier.push_back(&next);
			}
		}
		if (destination != nullptr) {
			army.clear_path();
			army.set_location(*destination);
		}
	}
}
