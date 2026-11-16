#pragma once

#include <any>
#include <concepts>
#include <cstdint>
#include <string_view>
#include <type_traits>
#include <variant>

#include <function2/function2.hpp>

#include "openvic-simulation/country/CountryInstance.hpp"
#include "openvic-simulation/diplomacy/CountryRelation.hpp"
#include "openvic-simulation/types/IdentifierRegistry.hpp"

namespace OpenVic {
	struct DiplomacyDefines;
	struct InstanceManager;

	struct DiplomaticActionType {
		friend struct DiplomaticActionManager;
		friend struct CancelableDiplomaticActionType;

		struct Argument {
			InstanceManager& instance_manager;
			CountryInstance* sender;
			CountryInstance* receiver;
			std::any context_data;
		};

		using allowed_to_commit_func = fu2::function_view<bool(Argument const&) const>;
		using get_acceptance_func = fu2::function_view<std::int32_t(Argument const&) const>;
		using commit_action_func = fu2::function_view<void(Argument&) const>;

		static bool allowed_to_commit_default(Argument const& argument) {
			return true;
		}

		static std::int32_t get_acceptance_default(Argument const& argument) {
			return 1;
		}

		struct Initializer {
			CountryRelationManager::influence_value_type influence_cost = 0;
			commit_action_func commit;
			allowed_to_commit_func allowed = allowed_to_commit_default;
			get_acceptance_func get_acceptance = get_acceptance_default;
		};

		const CountryRelationManager::influence_value_type influence_cost = 0;
		const commit_action_func commit_action_caller;
		const allowed_to_commit_func allowed_to_commit = allowed_to_commit_default;
		const get_acceptance_func get_acceptance = get_acceptance_default;

		void commit_action(Argument& arg) const {
			commit_action_caller(arg);
		}

	private:
		DiplomaticActionType(Initializer&& initializer);
	};

	struct CancelableDiplomaticActionType : DiplomaticActionType {
		friend struct DiplomaticActionManager;

		using allowed_to_cancel_func = fu2::function_view<bool(Argument const&)>;

		static bool allowed_to_cancel_default(Argument const& argument) {
			return true;
		}

		struct Initializer {
			CountryRelationManager::influence_value_type influence_cost = 0;
			commit_action_func commit;
			allowed_to_commit_func allowed = allowed_to_commit_default;
			get_acceptance_func get_acceptance = get_acceptance_default;
			allowed_to_cancel_func allowed_cancel = allowed_to_cancel_default;

			operator DiplomaticActionType::Initializer() {
				return { influence_cost, commit, allowed, get_acceptance };
			}
		};

		const allowed_to_cancel_func allowed_to_cancel = allowed_to_cancel_default;

	private:
		CancelableDiplomaticActionType(Initializer&& initializer);
	};

	struct DiplomaticActionTickCache;

	struct DiplomaticActionTypeStorage : std::variant<DiplomaticActionType, CancelableDiplomaticActionType>, HasIdentifier {
		using base_type = std::variant<DiplomaticActionType, CancelableDiplomaticActionType>;

		template<typename T>
		constexpr DiplomaticActionTypeStorage(std::string_view identifier, T&& t) : HasIdentifier(identifier), base_type(t) {}

		template<class Visitor>
		constexpr decltype(auto) visit(Visitor&& vis) {
			return std::visit(std::forward<Visitor>(vis), *this);
		}

		template<class Visitor>
		constexpr decltype(auto) visit(Visitor&& vis) const {
			return std::visit(std::forward<Visitor>(vis), *this);
		}

		constexpr bool is_cancelable() const {
			return visit([](auto&& arg) -> bool {
				using T = std::decay_t<decltype(arg)>;
				if constexpr (std::same_as<T, CancelableDiplomaticActionType>) {
					return true;
				} else {
					return false;
				}
			});
		}
	};

	struct DiplomaticActionTickCache {
		DiplomaticActionType::Argument argument;
		DiplomaticActionTypeStorage const* type = nullptr;
		bool allowed_to_commit = false;
		std::int32_t acceptance = -1;
	};

	struct DiplomaticActionManager {
	private:
		IdentifierRegistry<DiplomaticActionTypeStorage> IDENTIFIER_REGISTRY(diplomatic_action_type);
		DiplomacyDefines const* defines = nullptr;

		// The proposal is accepted (the action is done if it still can be) or declined: the relations and the news (version 96)
		void finish_proposal(CountryRelationManager::proposal_t proposal, bool accept, InstanceManager& instance_manager) const;

	public:
		// The armies of the country in the land of from that may not stand there go to the nearest province where they may (version 97)
		static void expel_armies(InstanceManager& instance_manager, CountryInstance& country, CountryInstance const& from);
		// What an action costs the sender in diplomatic points, what it changes in the relation of the two when it is accepted or declined, and
		// whether it needs the consent of the receiver (a country of a player then answers it himself: a proposal); from defines.lua (version 96)
		struct action_terms_t {
			fixed_point_t cost = fixed_point_t::_0;
			std::int32_t on_accept = 0;
			std::int32_t on_decline = 0;
			bool needs_consent = false;
			// a refusal costs the receiver DISHONORED_CALLALLY_PRESTIGE_PENALTY of prestige (the call to arms, version 97)
			bool dishonour = false;
		};
		action_terms_t get_terms(std::string_view identifier) const;
		action_terms_t get_terms(std::size_t action_type_index) const;
		// The index of an action (none if there is no such action)
		std::optional<std::size_t> get_action_index(std::string_view identifier) const;
		// A proposal of the AI to the country of a player (free for the AI, like its alliances between AI countries); false when the same
		// proposal waits already or the receiver declined it not long ago
		bool propose(
			std::size_t action_type_index, CountryInstance* sender, CountryInstance* receiver, InstanceManager& instance_manager,
			bool respect_cooldown = true
		) const;
		// The calls to arms of a new war to the allies that are countries of players (version 97: they answer, the AI allies join at once)
		void call_player_allies(InstanceManager& instance_manager, CountryInstance& leader) const;
		// The answer of the receiver to a proposal (false if there is no such proposal to him)
		bool answer_proposal(std::uint32_t id, CountryInstance const& receiver, bool accept, InstanceManager& instance_manager) const;
		// Every day: the proposals older than DIPLOMATIC_PROPOSAL_DAYS are declined; the war subsidies are paid (WARSUBSIDIES_PERCENT of the
		// military spending of the receiver of the day) and end when the receiver is at peace (version 97)
		void daily_update(InstanceManager& instance_manager) const;

		constexpr DiplomaticActionManager() {};

		bool add_diplomatic_action(std::string_view identifier, DiplomaticActionType::Initializer&& initializer);
		bool add_cancelable_diplomatic_action(
			std::string_view identifier, CancelableDiplomaticActionType::Initializer&& initializer
		);

		DiplomaticActionTickCache create_diplomatic_action_tick(
			std::string_view identifier, CountryInstance* sender, CountryInstance* receiver, std::any context_data,
			InstanceManager& instance_manager
		) const;

		// Sends the action from sender to receiver, asks the receiver whether it accepts and, if it does, commits it.
		// Returns true if the action was committed.
		// The receiver's decision currently lives in this function only (see the policy comment in the .cpp file).
		bool execute_diplomatic_action(
			std::size_t action_type_index, CountryInstance* sender, CountryInstance* receiver, std::any context_data,
			InstanceManager& instance_manager
		) const;

		bool setup_diplomatic_actions(DiplomacyDefines const& defines);
	};
}
