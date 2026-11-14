#include "PopDynamics.hpp"

#include <optional>
#include <algorithm>
#include <unordered_map>
#include <vector>

#include "openvic-simulation/DefinitionManager.hpp"
#include "openvic-simulation/InstanceManager.hpp"
#include "openvic-simulation/country/CountryInstance.hpp"
#include "openvic-simulation/economy/production/FactoryInstance.hpp"
#include "openvic-simulation/economy/production/ProductionType.hpp"
#include "openvic-simulation/economy/production/ResourceGatheringOperation.hpp"
#include "openvic-simulation/map/MapInstance.hpp"
#include "openvic-simulation/map/ProvinceInstance.hpp"
#include "openvic-simulation/map/State.hpp"
#include "openvic-simulation/population/Culture.hpp"
#include "openvic-simulation/population/Pop.hpp"
#include "openvic-simulation/population/PopManager.hpp"
#include "openvic-simulation/population/PopType.hpp"
#include "openvic-simulation/modifier/ModifierEffectCache.hpp"
#include "openvic-simulation/military/UnitModifiers.hpp"
#include "openvic-simulation/population/PopulationGrowth.hpp"
#include "openvic-simulation/utility/Logger.hpp"

using namespace OpenVic;

namespace {
	struct ProvinceScore {
		fixed_point_t unemployment;
		fixed_point_t life_needs;
		fixed_point_t score; // 0..1, higher is better
		bool has_pops = false;
		// The sums the score of the region (state) is made of
		fixed_point_t total, unemployable_total, unemployed, life_sum;
	};

	// Whole people from a fraction: the whole part is returned and removed from the accumulator
	std::int32_t take_whole(fixed_point_t& accumulator) {
		const std::int32_t whole = accumulator.floor<std::int32_t>();
		if (whole > 0) {
			accumulator -= whole;
		}
		return std::max(whole, 0);
	}

	// The people that move to another pop take their literacy with them (version 85): the literacy of the target becomes the average by the size
	// (a new pop of the changes of the population started with the literacy of a new pop, 10 %, and the literacy of the countries fell)
	void blend_literacy(Pop& target, Pop const& source, const std::int32_t count) {
		const std::int32_t before = std::max(0, type_safe::get(target.get_size()));
		if (count <= 0 || &target == &source) {
			return;
		}
		target.set_literacy((target.get_literacy() * before + source.get_literacy() * count) / (before + count));
	}

	// A literate man counts as 1 - LITERACY_PROMOTION_WEIGHT of the share that may rise to another strata (version 89, docs/LITERACY.md): the pop
	// rises 1 / (1 - weight x literacy) times faster (twice at 100 % with the weight 0.5)
	fixed_point_t literacy_promotion_factor(OpenVicPopulationDefines const& defines, const fixed_point_t literacy) {
		const fixed_point_t cost = fixed_point_t::_1 - defines.literacy_promotion_weight.of(std::clamp(literacy, fixed_point_t::_0, fixed_point_t::_1));
		return cost > fixed_point_t::_0 ? fixed_point_t::_1 / cost : fixed_point_t::_1;
	}

	// Pop of the same type, culture and religion in the province, a new pop if there is none
	Pop* find_or_create_pop(
		ProvinceInstance& province, PopType const& type, Culture const& culture, Pop const& template_pop, PopDeps const& deps
	) {
		for (Pop& pop : province.get_mutable_pops()) {
			if (&pop.get_type() == &type && &pop.culture == &culture && &pop.religion == &template_pop.religion
				&& pop.get_rebel_type() == nullptr) {
				return &pop;
			}
		}
		return province.add_pop(static_cast<PopBase const&>(template_pop).with_changes(type, culture, pop_size_t(0)), deps);
	}

	ProvinceScore compute_score(ProvinceInstance& province, OpenVicPopulationDefines const& defines) {
		ProvinceScore result;
		fixed_point_t total = fixed_point_t::_0;
		fixed_point_t unemployable_total = fixed_point_t::_0;
		fixed_point_t unemployed = fixed_point_t::_0;
		fixed_point_t life_sum = fixed_point_t::_0;

		for (Pop& pop : province.get_mutable_pops()) {
			if (pop.get_rebel_type() != nullptr || type_safe::get(pop.get_size()) <= 0) {
				continue;
			}
			const fixed_point_t size = type_safe::get(pop.get_size());
			total += size;
			life_sum += pop.get_life_needs_fulfilled() * size;
			if (pop.get_type().can_be_unemployed) {
				unemployable_total += size;
				unemployed += size * pop.get_unemployment_fraction();
			}
		}

		result.total = total;
		result.unemployable_total = unemployable_total;
		result.unemployed = unemployed;
		result.life_sum = life_sum;
		if (total > fixed_point_t::_0) {
			result.has_pops = true;
			result.life_needs = life_sum / total;
			result.unemployment = unemployable_total > fixed_point_t::_0 ? unemployed / unemployable_total : fixed_point_t::_0;
			result.score = defines.score_unemployment_weight.of(fixed_point_t::_1 - result.unemployment) + defines.score_life_needs_weight.of(result.life_needs);
		}
		return result;
	}
}

std::int32_t PopDynamics::job_migration_take(
	OpenVicPopulationDefines const& defines, const std::int32_t unemployed, const std::int32_t employed, const bool pays_more, const std::int32_t pop_size,
	const std::int32_t room
) {
	std::int32_t take = 0;
	if (unemployed > 0) {
		take += std::max<std::int32_t>(1, static_cast<std::int32_t>(defines.job_migration_unemployed_share.of_int(unemployed)));
	}
	if (pays_more && employed > 0) {
		take += static_cast<std::int32_t>(defines.job_migration_profit_share.of_int(employed));
	}
	return std::max(0, std::min({ take, pop_size - 1, room }));
}

void PopDynamics::monthly_update(InstanceManager& instance_manager, PopDeps const& pop_deps) {
	OpenVicPopulationDefines const& defines =
		instance_manager.definition_manager.get_define_manager().get_openvic_defines().population;
	MapInstance& map_instance = instance_manager.get_map_instance();
	PopManager const& pop_manager = instance_manager.definition_manager.get_pop_manager();

	PopType const* const farmers = pop_manager.get_pop_type_by_identifier("farmers");
	PopType const* const labourers = pop_manager.get_pop_type_by_identifier("labourers");
	PopType const* const craftsmen = pop_manager.get_pop_type_by_identifier("craftsmen");
	PopType const* const artisans = pop_manager.get_pop_type_by_identifier("artisans");
	PopType const* const clergymen = pop_manager.get_pop_type_by_identifier("clergymen");
	PopType const* const clerks = pop_manager.get_pop_type_by_identifier("clerks");
	PopType const* const slaves = pop_manager.get_pop_type_by_identifier("slaves");
	PopsDefines const& pops_defines = instance_manager.definition_manager.get_define_manager().get_pops_defines();
	// The free jobs of the RGOs the hungry artisans of this month took (by province)
	std::unordered_map<ProvinceInstance const*, std::int32_t> rgo_jobs_taken;

	PopType const* const bureaucrats = pop_manager.get_pop_type_by_identifier("bureaucrats");
	if (bureaucrats != nullptr) {
		// The corruption of the laws (docs/LAWS_DESIGN.md, hereditary bureaucrats): the taxes the bureaucrats kept this month go from the treasury to
		// the bureaucrats of the country, by their size
		std::unordered_map<CountryInstance*, std::int64_t> bureaucrat_men;
		for (ProvinceInstance& province : map_instance.get_province_instances()) {
			CountryInstance* const country = province.get_country_to_report_economy();
			if (country == nullptr || country->get_corruption_owed() <= fixed_point_t::_0) {
				continue;
			}
			for (Pop const& pop : province.get_pops()) {
				if (&pop.get_type() == bureaucrats) {
					bureaucrat_men[country] += type_safe::get(pop.get_size());
				}
			}
		}
		std::unordered_map<CountryInstance*, fixed_point_t> corruption_pool;
		for (CountryInstance& country : instance_manager.get_country_instance_manager().get_country_instances()) {
			const auto men = bureaucrat_men.find(&country);
			const fixed_point_t owed = country.get_corruption_owed();
			country.set_corruption_owed(fixed_point_t::_0);
			if (owed <= fixed_point_t::_0 || men == bureaucrat_men.end() || men->second <= 0 || men->second > std::numeric_limits<std::int32_t>::max()) {
				continue;
			}
			const fixed_point_t amount = std::min(owed, country.get_cash_stockpile().load());
			if (amount > fixed_point_t::_0) {
				country.spend_cash(amount);
				corruption_pool[&country] = amount;
			}
		}
		for (ProvinceInstance& province : map_instance.get_province_instances()) {
			CountryInstance* const country = province.get_country_to_report_economy();
			const auto pool = country != nullptr ? corruption_pool.find(country) : corruption_pool.end();
			if (pool == corruption_pool.end()) {
				continue;
			}
			const std::int32_t total = static_cast<std::int32_t>(bureaucrat_men[country]);
			for (Pop& pop : province.get_mutable_pops()) {
				if (&pop.get_type() == bureaucrats && pop.get_size() > pop_size_t(0)) {
					const fixed_point_t share = pool->second * static_cast<std::int32_t>(type_safe::get(pop.get_size())) / total;
					if (share > fixed_point_t::_0) {
						pop.add_government_salary_administration(share);
					}
				}
			}
		}

		// The bureaucrats come from the literate poor (version 82): while the bureaucrats of a state are fewer than the share it needs (version 88:
		// the share of the state, docs/ADMINISTRATION.md; before, the share the country wants), BUREAUCRAT_PROMOTION_SHARE x (1 + bureaucrat_promotion
		// of the laws) x the factor of their pay x (1 + ADMIN_FOCUS_PROMOTION in the state of the priority) of the men of its farmers, labourers and
		// craftsmen, times their literacy, become bureaucrats every month (of their culture, in their province)
		OpenVicEconomyDefines const& economy_defines = instance_manager.definition_manager.get_define_manager().get_openvic_defines().economy;
		if (defines.bureaucrat_promotion_share.numerator > 0 && farmers != nullptr && labourers != nullptr && craftsmen != nullptr) {
			std::unordered_map<State const*, std::pair<std::int64_t, std::int64_t>> state_men; // bureaucrats, all
			for (ProvinceInstance& province : map_instance.get_province_instances()) {
				if (province.get_state() == nullptr || province.get_owner() == nullptr) {
					continue;
				}
				auto& men = state_men[province.get_state()];
				for (Pop const& pop : province.get_pops()) {
					men.first += &pop.get_type() == bureaucrats ? type_safe::get(pop.get_size()) : 0;
					men.second += type_safe::get(pop.get_size());
				}
			}
			for (ProvinceInstance& province : map_instance.get_province_instances()) {
				CountryInstance const* const owner = province.get_owner();
				State const* const state = province.get_state();
				if (owner == nullptr || state == nullptr) {
					continue;
				}
				auto const& men = state_men[state];
				const fixed_point_t wanted = state->get_admin_needed_share();
				if (men.second <= 0 || wanted <= fixed_point_t::_0
					|| fixed_point_t(static_cast<std::int32_t>(std::min<std::int64_t>(men.first * 10000 / men.second, 10000))) / 10000 >= wanted) {
					continue;
				}
				fixed_point_t share = defines.bureaucrat_promotion_share.fixed()
					* std::max(fixed_point_t::_0, fixed_point_t::_1 + owner->get_law_effect(law_effect_t::bureaucrat_promotion))
					* owner->get_bureaucrat_promotion_pay_factor();
				if (owner->get_admin_focus_state() == state) {
					share = share * (fixed_point_t::_1 + economy_defines.admin_focus_promotion.fixed());
				}
				if (share <= fixed_point_t::_0) {
					continue;
				}
				std::vector<Pop*> candidates;
				for (Pop& pop : province.get_mutable_pops()) {
					PopType const* const type = &pop.get_type();
					if ((type == farmers || type == labourers || type == craftsmen) && pop.get_size() > pop_size_t(0) && pop.get_rebel_type() == nullptr) {
						candidates.push_back(&pop);
					}
				}
				for (Pop* pop : candidates) {
					pop->migration_remainder += share * pop->get_literacy() * literacy_promotion_factor(defines, pop->get_literacy())
						* static_cast<std::int32_t>(type_safe::get(pop->get_size()));
					const std::int32_t whole = std::min(take_whole(pop->migration_remainder), type_safe::get(pop->get_size()));
					if (whole > 0) {
						if (Pop* target = find_or_create_pop(province, *bureaucrats, pop->culture, *pop, pop_deps)) {
							blend_literacy(*target, *pop, whole);
							pop->change_size(-whole, 1);
							target->change_size(whole, 1);
						}
					}
				}
			}
		}
	}

	// The officers come from the literate (version 89, docs/LITERACY.md): while the officers of a country are fewer than OFFICERS_PER_SOLDIER of its
	// soldiers, OFFICER_PROMOTION_SHARE x the literacy factor of the literate men of its middle and rich pops (the poor too with the unlock
	// mechanic:poor_officers) become officers every month, in their province; not the officers, the bureaucrats and the slaves
	if (PopType const* const officers = pop_manager.get_pop_type_by_identifier("officers"),
		* const soldiers = pop_manager.get_pop_type_by_identifier("soldiers");
		officers != nullptr && soldiers != nullptr && defines.officer_promotion_share.numerator > 0) {
		std::unordered_map<CountryInstance const*, std::pair<std::int64_t, std::int64_t>> army_men; // officers, soldiers
		for (ProvinceInstance const& province : map_instance.get_province_instances()) {
			CountryInstance const* const owner = province.get_owner();
			if (owner == nullptr) {
				continue;
			}
			for (Pop const& pop : province.get_pops()) {
				if (&pop.get_type() == officers) {
					army_men[owner].first += type_safe::get(pop.get_size());
				} else if (&pop.get_type() == soldiers) {
					army_men[owner].second += type_safe::get(pop.get_size());
				}
			}
		}
		for (ProvinceInstance& province : map_instance.get_province_instances()) {
			CountryInstance const* const owner = province.get_owner();
			if (owner == nullptr) {
				continue;
			}
			auto const& men = army_men[owner];
			if (men.second <= 0 || men.first * Rate::DENOMINATOR >= men.second * defines.officers_per_soldier.numerator) {
				continue;
			}
			const bool poor_officers = owner->has_unlock("mechanic:poor_officers");
			std::vector<Pop*> candidates;
			for (Pop& pop : province.get_mutable_pops()) {
				PopType const* const type = &pop.get_type();
				if (type == officers || type == bureaucrats || type == slaves || type->is_slave || pop.get_size() <= pop_size_t(0)
					|| pop.get_rebel_type() != nullptr) {
					continue;
				}
				if (type->strata.get_identifier() == "poor" && !poor_officers) {
					continue;
				}
				candidates.push_back(&pop);
			}
			for (Pop* pop : candidates) {
				const fixed_point_t literacy = std::clamp(pop->get_literacy(), fixed_point_t::_0, fixed_point_t::_1);
				pop->migration_remainder += defines.officer_promotion_share.fixed() * literacy * literacy_promotion_factor(defines, literacy)
					* static_cast<std::int32_t>(type_safe::get(pop->get_size()));
				const std::int32_t whole = std::min(take_whole(pop->migration_remainder), type_safe::get(pop->get_size()));
				if (whole > 0) {
					if (Pop* target = find_or_create_pop(province, *officers, pop->culture, *pop, pop_deps)) {
						blend_literacy(*target, *pop, whole);
						pop->change_size(-whole, 1);
						target->change_size(whole, 1);
					}
				}
			}
		}
	}

	// The maximum organisation of the regiments by the literacy of their soldiers (version 89, docs/LITERACY.md)
	instance_manager.get_unit_instance_manager().update_literacy_organisation(
		instance_manager.definition_manager.get_define_manager().get_openvic_defines().war.literacy_organisation
	);

	// The bureaucrats paid under ADMIN_LOW_PAY leave (version 88, docs/ADMINISTRATION.md): ADMIN_LOW_PAY_LEAVE x (1 - pay / ADMIN_LOW_PAY) of them
	// become artisans of their province every month
	if (bureaucrats != nullptr && artisans != nullptr) {
		OpenVicEconomyDefines const& economy_defines = instance_manager.definition_manager.get_define_manager().get_openvic_defines().economy;
		const fixed_point_t low = economy_defines.admin_low_pay.fixed();
		if (low > fixed_point_t::_0 && economy_defines.admin_low_pay_leave.numerator > 0) {
			for (ProvinceInstance& province : map_instance.get_province_instances()) {
				CountryInstance const* const owner = province.get_owner();
				if (owner == nullptr || owner->get_administration_pay() >= low) {
					continue;
				}
				const fixed_point_t share = economy_defines.admin_low_pay_leave.of(fixed_point_t::_1 - owner->get_administration_pay() / low);
				std::vector<Pop*> leaving;
				for (Pop& pop : province.get_mutable_pops()) {
					if (&pop.get_type() == bureaucrats && pop.get_size() > pop_size_t(0)) {
						leaving.push_back(&pop);
					}
				}
				for (Pop* pop : leaving) {
					pop->migration_remainder += share * static_cast<std::int32_t>(type_safe::get(pop->get_size()));
					const std::int32_t whole = std::min(take_whole(pop->migration_remainder), type_safe::get(pop->get_size()));
					if (whole > 0) {
						if (Pop* target = find_or_create_pop(province, *artisans, pop->culture, *pop, pop_deps)) {
							blend_literacy(*target, *pop, whole);
							pop->change_size(-whole, 2);
							target->change_size(whole, 2);
						}
					}
				}
			}
		}
	}

	// The money goes round: a pop that holds more than SAVINGS_MAX_DAYS days of what it earns or spends in a day (today's income and spending: the
	// month starts after the market) passes SAVINGS_SHARE of the rest to the pops of its state that hold less, by their size (the pay for their
	// services); a state with nobody to pay puts it into the investment fund of its country. The states one by one, in the order of the map
	if (defines.savings_max_days > 0 && defines.savings_share.numerator > 0) {
		// What a pop keeps: SAVINGS_MAX_DAYS days of what it earns or spends, times the savings of the laws (women own property)
		const auto savings_keep = [&defines](Pop const& pop) -> fixed_point_t {
			const fixed_point_t keep = std::max(pop.get_income(), pop.get_expenses().get_copy_of_value()) * defines.savings_max_days;
			CountryInstance const* const country = pop.get_location().get_country_to_report_economy();
			return country != nullptr ? keep * std::max(fixed_point_t::_0, fixed_point_t::_1 + country->get_law_effect(law_effect_t::savings_threshold)) : keep;
		};
		std::unordered_set<State const*> states_done;
		std::vector<Pop*> givers, takers;
		for (ProvinceInstance& first_province : map_instance.get_province_instances()) {
			State* const state = first_province.get_state();
			if (state == nullptr || !states_done.insert(state).second) {
				continue;
			}
			givers.clear();
			takers.clear();
			std::int64_t taker_people = 0;
			for (ProvinceInstance& province : state->get_provinces()) {
				if (province.get_country_to_report_economy() == nullptr) {
					continue;
				}
				for (Pop& pop : province.get_mutable_pops()) {
					if (pop.get_size() <= pop_size_t(0)) {
						continue;
					}
					const fixed_point_t keep = savings_keep(pop);
					if (pop.get_cash().get_copy_of_value() > keep) {
						givers.push_back(&pop);
					} else {
						takers.push_back(&pop);
						taker_people += type_safe::get(pop.get_size());
					}
				}
			}
			fixed_point_t pool = fixed_point_t::_0;
			for (Pop* pop : givers) {
				const fixed_point_t keep = savings_keep(*pop);
				const fixed_point_t amount = defines.savings_share.of(pop->get_cash().get_copy_of_value() - keep);
				if (amount > fixed_point_t::_0) {
					pool += pop->take_cash(amount);
				}
			}
			if (pool <= fixed_point_t::_0) {
				continue;
			}
			CountryInstance* const country = givers.front()->get_location().get_country_to_report_economy();
			if (taker_people <= 0 || taker_people > std::numeric_limits<std::int32_t>::max()) {
				if (country != nullptr) {
					country->add_savings_to_investment_fund(pool);
				}
				continue;
			}
			// SAVINGS_FUND_SHARE into the investment fund, SAVINGS_TREASURY_SHARE into the treasury, the rest to the pops
			if (country != nullptr) {
				const fixed_point_t to_fund = defines.savings_fund_share.of(pool);
				const fixed_point_t to_treasury = std::min(pool - to_fund, defines.savings_treasury_share.of(pool));
				if (to_fund > fixed_point_t::_0) {
					country->add_savings_to_investment_fund(to_fund);
				}
				if (to_treasury > fixed_point_t::_0) {
					country->add_cash(to_treasury);
				}
				pool -= to_fund + to_treasury;
				if (pool <= fixed_point_t::_0) {
					continue;
				}
			}
			// The shares by size; what the rounding leaves goes to the biggest taker
			fixed_point_t paid = fixed_point_t::_0;
			Pop* biggest = takers.front();
			for (Pop* pop : takers) {
				if (pop->get_size() > biggest->get_size()) {
					biggest = pop;
				}
				const fixed_point_t share = pool * static_cast<std::int32_t>(type_safe::get(pop->get_size())) / static_cast<std::int32_t>(taker_people);
				if (share > fixed_point_t::_0) {
					pop->add_services_income(share);
					paid += share;
				}
			}
			if (pool > paid) {
				biggest->add_services_income(pool - paid);
			}
		}
	}

	// Scores of the provinces before anything moves
	std::unordered_map<ProvinceInstance const*, ProvinceScore> scores;
	for (ProvinceInstance& province : map_instance.get_province_instances()) {
		if (!province.province_definition.is_water() && province.get_pop_count() > 0) {
			scores[&province] = compute_score(province, defines);
		}
	}

	// The scores of the regions (states): the people move between regions, a move between the provinces of one region means nothing
	std::unordered_map<State const*, ProvinceScore> state_scores;
	for (auto const& [province_ptr, province_score] : scores) {
		State const* const score_state = province_ptr->get_state();
		if (score_state == nullptr) {
			continue;
		}
		ProvinceScore& sum = state_scores[score_state];
		sum.total += province_score.total;
		sum.unemployable_total += province_score.unemployable_total;
		sum.unemployed += province_score.unemployed;
		sum.life_sum += province_score.life_sum;
	}
	for (auto& [score_state, sum] : state_scores) {
		if (sum.total > fixed_point_t::_0) {
			sum.has_pops = true;
			sum.life_needs = sum.life_sum / sum.total;
			sum.unemployment = sum.unemployable_total > fixed_point_t::_0 ? sum.unemployed / sum.unemployable_total : fixed_point_t::_0;
			sum.score = defines.score_unemployment_weight.of(fixed_point_t::_1 - sum.unemployment)
				+ defines.score_life_needs_weight.of(sum.life_needs);
		}
	}
	const auto region_score = [&scores, &state_scores](ProvinceInstance const* province) -> ProvinceScore {
		if (province->get_state() != nullptr) {
			const auto it = state_scores.find(province->get_state());
			if (it != state_scores.end()) {
				return it->second;
			}
		}
		const auto it = scores.find(province);
		return it != scores.end() ? it->second : ProvinceScore {};
	};

	// Free jobs of the open factories of each state, and what they paid a worker yesterday (nullopt: they have nobody yet)
	std::unordered_map<State const*, std::int32_t> vacancies;
	std::unordered_map<State const*, std::optional<fixed_point_t>> factory_pay;
	for (ProvinceInstance& province : map_instance.get_province_instances()) {
		State* const state = province.get_state();
		if (state == nullptr || vacancies.contains(state)) {
			continue;
		}
		std::int32_t free_jobs = 0, employees = 0;
		fixed_point_t wages = fixed_point_t::_0;
		for (FactoryInstance const& factory : state->get_factories()) {
			if (factory.is_open() && !factory.is_under_construction()) {
				free_jobs += std::max(0, type_safe::get(factory.get_max_employee_count()) - type_safe::get(factory.get_employee_count()));
				employees += type_safe::get(factory.get_employee_count());
				wages += factory.get_wages_yesterday();
			}
		}
		vacancies[state] = free_jobs;
		factory_pay[state] = employees > 0 ? std::optional<fixed_point_t> { wages / employees } : std::nullopt;
	}

	// Free jobs of the resource gathering operations by country and pop type: where the surplus people can go
	struct Vacancy {
		ProvinceInstance* province;
		std::int32_t free_jobs;
	};
	const std::size_t country_count = instance_manager.get_country_instance_manager().get_country_instances().size();
	const std::size_t type_count = static_cast<std::size_t>(pop_manager.get_pop_type_count());
	std::vector<std::vector<Vacancy>> job_vacancies(country_count * type_count);
	std::vector<std::int64_t> free_jobs_total(country_count * type_count, 0);
	for (ProvinceInstance& province : map_instance.get_province_instances()) {
		CountryInstance const* const rgo_owner = province.get_owner();
		if (rgo_owner == nullptr || province.province_definition.is_water()) {
			continue;
		}
		ProductionType const* const production_type = province.get_rgo().get_production_type_nullable();
		if (production_type == nullptr) {
			continue;
		}
		const std::int32_t free_jobs = type_safe::get(province.get_rgo().get_max_employee_count_cache())
			- type_safe::get(province.get_rgo().get_total_employees_count_cache());
		if (free_jobs <= 0) {
			continue;
		}
		for (Job const& job : production_type->get_jobs()) {
			const std::size_t slot = type_safe::get(rgo_owner->index) * type_count + type_safe::get(job.pop_type_index);
			job_vacancies[slot].push_back({ &province, free_jobs });
			free_jobs_total[slot] += free_jobs;
		}
	}
	for (std::vector<Vacancy>& list : job_vacancies) {
		std::sort(list.begin(), list.end(), [](Vacancy const& lhs, Vacancy const& rhs) -> bool {
			if (lhs.free_jobs != rhs.free_jobs) {
				return lhs.free_jobs > rhs.free_jobs;
			}
			return type_safe::get(lhs.province->province_definition.index) < type_safe::get(rhs.province->province_definition.index);
		});
	}

	// The free jobs of the RGOs by province, with the type of the people the job is for and what the RGO paid a worker yesterday (the places of the job migration)
	struct RgoPlace {
		std::int32_t free_jobs = 0;
		PopType const* job_type = nullptr;
		fixed_point_t income_per_worker;
	};
	const auto rgo_income_per_worker = [](ResourceGatheringOperation const& rgo) -> fixed_point_t {
		const std::int32_t employees = type_safe::get(rgo.get_total_employees_count_cache());
		return employees > 0 ? rgo.get_revenue_yesterday() / employees : fixed_point_t::_0;
	};
	std::unordered_map<ProvinceInstance const*, RgoPlace> rgo_places;
	// Only the people of the types that work in RGOs (farmers, labourers) take part: an unemployed clerk does not become a farmer
	std::unordered_map<PopType const*, bool> rgo_worker_types;
	if (defines.job_migration_enabled != 0) {
		for (ProvinceInstance& province : map_instance.get_province_instances()) {
			if (province.get_owner() == nullptr || province.province_definition.is_water()) {
				continue;
			}
			ResourceGatheringOperation const& rgo = province.get_rgo();
			ProductionType const* const production_type = rgo.get_production_type_nullable();
			if (production_type == nullptr) {
				continue;
			}
			const std::int32_t free_jobs = type_safe::get(rgo.get_max_employee_count_cache()) - type_safe::get(rgo.get_total_employees_count_cache());
			if (free_jobs <= 0) {
				continue;
			}
			// The workers of the RGO: the job with most people (the owner is not a job)
			PopType const* job_type = nullptr;
			fixed_point_t most = fixed_point_t::_0;
			for (Job const& job : production_type->get_jobs()) {
				PopType const* const candidate = pop_manager.get_pop_type_by_index(job.pop_type_index);
				if (candidate != nullptr && candidate->can_be_unemployed && !candidate->is_slave && job.amount > most) {
					job_type = candidate;
					most = job.amount;
				}
			}
			if (job_type != nullptr) {
				rgo_places[&province] = { free_jobs, job_type, rgo_income_per_worker(rgo) };
			}
			for (Job const& job : production_type->get_jobs()) {
				if (PopType const* const worker_type = pop_manager.get_pop_type_by_index(job.pop_type_index); worker_type != nullptr && !worker_type->is_slave) {
					rgo_worker_types[worker_type] = true;
				}
			}
		}
	}
	std::int32_t job_migrated_total = 0, job_retrained_total = 0;

	// Moves people of a pop to the free jobs of a country (best places first), returns how many moved
	const auto move_to_vacancies = [&](
		Pop& source, ProvinceInstance& source_province, const std::size_t slot, std::int32_t count, const std::int32_t kind
	) -> std::int32_t {
		std::int32_t moved = 0;
		for (Vacancy& vacancy : job_vacancies[slot]) {
			if (count <= 0) {
				break;
			}
			if (vacancy.free_jobs <= 0 || vacancy.province == &source_province) {
				continue;
			}
			// Not more than a share of the free jobs of a place in a month
			const std::int32_t room = std::max<std::int32_t>(1, static_cast<std::int32_t>(defines.immigration_fill_share.of_int(vacancy.free_jobs)));
			const std::int32_t take = std::min({ count, room, vacancy.free_jobs });
			Pop* const target = find_or_create_pop(*vacancy.province, source.get_type(), source.culture, source, pop_deps);
			if (target == nullptr) {
				continue;
			}
			blend_literacy(*target, source, take);
			source.change_size(-take, kind);
			target->change_size(take, kind);
			vacancy.free_jobs -= take;
			free_jobs_total[slot] -= take;
			count -= take;
			moved += take;
		}
		return moved;
	};

	// The data of the growth, the year and the multipliers of the countries (medicine, sanitation), worked out once for the month
	PopulationGrowthData const& growth_data = pop_manager.get_growth_data();
	const std::int32_t year = static_cast<std::int32_t>(instance_manager.get_today().get_year());
	ModifierEffectCache const& effect_cache = instance_manager.definition_manager.get_modifier_manager().get_modifier_effect_cache();
	std::unordered_map<CountryInstance const*, fixed_point_t> growth_multipliers;
	// The education of the countries: the funding times the efficiency (the techs and the laws)
	std::unordered_map<CountryInstance const*, fixed_point_t> education_factors;
	const fixed_point_t clergy_base = pops_defines.get_base_clergy_for_literacy();
	const fixed_point_t clergy_range = pops_defines.get_max_clergy_for_literacy() - clergy_base;
	const std::array<ScopeWeight, 1> all_weights { { { effect_cache.find_unit_scope("all"), fixed_point_t::_1 } } };

	std::int32_t starved_total = 0;
	std::int32_t emigrated_internal_total = 0;
	std::int32_t emigrated_external_total = 0;
	std::int32_t grown_total = 0;
	std::int32_t migrated_total = 0;
	std::int32_t assimilated_total = 0;
	std::int32_t promoted_total = 0;

	for (ProvinceInstance& province : map_instance.get_province_instances()) {
		if (province.province_definition.is_water() || province.get_pop_count() == 0) {
			continue;
		}
		CountryInstance const* const owner = province.get_owner();
		State* const state = province.get_state();

		// The pops are collected first, new pops added below must not be processed this month
		std::vector<Pop*> pops;
		for (Pop& pop : province.get_mutable_pops()) {
			pop.clear_dynamics_record();
			if (pop.get_rebel_type() == nullptr && type_safe::get(pop.get_size()) > 0) {
				pops.push_back(&pop);
			}
		}

		// The clergy of the province teaches (the literacy below): (its share - BASE_CLERGY_FOR_LITERACY) / (MAX - BASE), 0..1
		fixed_point_t clergy_factor = fixed_point_t::_0;
		if (clergymen != nullptr && owner != nullptr) {
			std::int64_t clergy_men = 0, all_men = 0;
			for (Pop const* pop : pops) {
				all_men += type_safe::get(pop->get_size());
				clergy_men += &pop->get_type() == clergymen ? type_safe::get(pop->get_size()) : 0;
			}
			if (all_men > 0 && clergy_range > fixed_point_t::_0) {
				const fixed_point_t share = fixed_point_t(static_cast<std::int32_t>(clergy_men * 100000 / all_men)) / 100000;
				clergy_factor = std::clamp((share - clergy_base) / clergy_range, fixed_point_t::_0, fixed_point_t::_1);
			}
		}

		// Possible destinations of migrants: the land neighbours of the same owner in other regions (the better region wins)
		std::vector<ProvinceInstance*> destinations;
		if (owner != nullptr) {
			for (ProvinceDefinition::adjacency_t const& adjacency : province.province_definition.get_adjacencies()) {
				if (adjacency.get_type() != ProvinceDefinition::adjacency_t::type_t::LAND || adjacency.get_to().is_water()) {
					continue;
				}
				ProvinceInstance& other = map_instance.get_province_instance_by_definition(adjacency.get_to());
				if (other.get_owner() == owner && other.get_state() != state
					&& std::find(destinations.begin(), destinations.end(), &other) == destinations.end()) {
					destinations.push_back(&other);
				}
			}
		}
		ProvinceInstance* best_destination = nullptr;
		const ProvinceScore own_score = region_score(&province);
		fixed_point_t best_score = own_score.score + defines.migration_score_margin.fixed();
		for (ProvinceInstance* destination : destinations) {
			const ProvinceScore destination_score = region_score(destination);
			if (!destination_score.has_pops) {
				continue;
			}
			if (destination_score.score > best_score) {
				best_score = destination_score.score;
				best_destination = destination;
			}
		}

		// The best free job in the region or the next region of the country: where the RGO paid a worker most yesterday
		ProvinceInstance* job_place_province = nullptr;
		if (defines.job_migration_enabled != 0 && owner != nullptr && !rgo_places.empty()) {
			fixed_point_t best_income = fixed_point_t(-1);
			const auto consider = [&](ProvinceInstance* candidate) -> void {
				if (candidate == &province || candidate->get_owner() != owner) {
					return;
				}
				const auto place = rgo_places.find(candidate);
				if (place == rgo_places.end() || place->second.free_jobs <= 0) {
					return;
				}
				if (place->second.income_per_worker > best_income) {
					best_income = place->second.income_per_worker;
					job_place_province = candidate;
				}
			};
			if (state != nullptr) {
				for (ProvinceInstance& in_region : state->get_provinces()) {
					consider(&in_region);
				}
			}
			for (ProvinceDefinition::adjacency_t const& adjacency : province.province_definition.get_adjacencies()) {
				if (adjacency.get_type() == ProvinceDefinition::adjacency_t::type_t::LAND && !adjacency.get_to().is_water()) {
					consider(&map_instance.get_province_instance_by_definition(adjacency.get_to()));
				}
			}
		}
		const fixed_point_t own_income = rgo_income_per_worker(province.get_rgo());

		for (Pop* pop : pops) {
			const std::int32_t size = type_safe::get(pop->get_size());
			if (size <= 0) {
				continue;
			}

			// ---- Growth: the base of the data (country, culture, region, year), by the life needs and the modifiers of the country; starvation shrinks the pop
			const fixed_point_t life = pop->get_life_needs_fulfilled();
			fixed_point_t base_yearly = growth_data.get_yearly_rate(
				owner != nullptr ? owner->get_identifier() : std::string_view {}, pop->culture.group.get_identifier(), pop->culture.get_identifier(),
				province.province_definition.has_region() ? province.province_definition.get_region()->get_identifier() : std::string_view {}, year,
				defines.growth_per_year.fixed()
			);
			fixed_point_t growth_multiplier = fixed_point_t::_1;
			if (owner != nullptr) {
				const auto cached = growth_multipliers.find(owner);
				if (cached != growth_multipliers.end()) {
					growth_multiplier = cached->second;
				} else {
					growth_multiplier = population_growth::growth_multiplier(
						defines, (effect_cache.get_population_growth_pct() != nullptr ? owner->get_modifier_effect_value(*effect_cache.get_population_growth_pct()) : fixed_point_t::_0)
							// and the laws (the health care, the women in the workplace)
							+ owner->get_law_effect(law_effect_t::population_growth),
						get_scoped_product(*owner, effect_cache, scoped_effect_t::CASUALTIES_TAKEN_MULT, all_weights)
					);
					growth_multipliers.emplace(owner, growth_multiplier);
				}
			}
			const fixed_point_t yearly_rate = population_growth::monthly_yearly_rate(defines, base_yearly, life, growth_multiplier);
			pop->growth_remainder += yearly_rate * size / 12;
			if (pop->growth_remainder >= fixed_point_t::_1) {
				const std::int32_t whole = take_whole(pop->growth_remainder);
				pop->change_size(whole, 0);
				grown_total += whole;
			} else if (pop->growth_remainder <= -fixed_point_t::_1) {
				// Negative accumulator: take the whole people away
				fixed_point_t positive = -pop->growth_remainder;
				const std::int32_t whole = take_whole(positive);
				pop->growth_remainder = -positive;
				// Hunger never empties a pop (code that divides by the size of a pop would break on an empty one)
				const std::int32_t can_go = std::max(0, type_safe::get(pop->get_size()) - 1);
				pop->change_size(-std::min(whole, can_go), 0);
				grown_total -= std::min(whole, can_go);
			}

			const PopType& type = pop->get_type();

			// ---- The literacy (version 85): the school of the clergy of the province, paid by the education budget (LITERACY_GROWTH_SCALE)
			if (owner != nullptr && !type.is_slave && pop->get_literacy() < fixed_point_t::_1 && defines.literacy_growth_scale.numerator > 0) {
				fixed_point_t education = fixed_point_t::_0;
				if (const auto cached = education_factors.find(owner); cached != education_factors.end()) {
					education = cached->second;
				} else {
					fixed_point_t efficiency = fixed_point_t::_1;
					if (effect_cache.get_education_efficiency() != nullptr) {
						efficiency += owner->get_modifier_effect_value(*effect_cache.get_education_efficiency());
					}
					if (effect_cache.get_education_efficiency_modifier() != nullptr) {
						efficiency += owner->get_modifier_effect_value(*effect_cache.get_education_efficiency_modifier());
					}
					education = owner->get_education_funding() * std::max(fixed_point_t::_0, efficiency);
					education_factors.emplace(owner, education);
				}
				// The law of the strata (no schools: only the clergy learns; private schools: the poor less...)
				fixed_point_t law = fixed_point_t::_1;
				if (&type != clergymen) {
					std::string_view const strata = type.strata.get_identifier();
					law = std::max(fixed_point_t::_0, fixed_point_t::_1 + owner->get_law_effect(
						strata == "rich" ? law_effect_t::literacy_rich : strata == "middle" ? law_effect_t::literacy_middle : law_effect_t::literacy_poor
					));
				}
				// The schools of the state do not need the clergy
				const fixed_point_t teachers = owner->get_law_effect(law_effect_t::secular_schools) > fixed_point_t::_0 ? fixed_point_t::_1 : clergy_factor;
				fixed_point_t growth = (fixed_point_t::_1 - pop->get_literacy()) * law * teachers * education * pops_defines.get_literacy_change_speed();
				growth = defines.literacy_growth_scale.of(growth);
				// (the principles of the sphere, version 94: the exchange of the teachers)
				if (const fixed_point_t principle = owner->get_sphere_principle_bonus(principle_effect_t::LITERACY); principle != fixed_point_t::_0) {
					growth *= fixed_point_t::_1 + principle;
				}
				if (pop->get_culture_status() == Pop::culture_status_t::UNACCEPTED) {
					growth = defines.literacy_unaccepted_share.of(growth);
				}
				if (growth > fixed_point_t::_0) {
					pop->set_literacy(std::min(fixed_point_t::_1, pop->get_literacy() + growth));
				}
			}

			// ---- Debt slavery (the laws): the hungry poor without money sell their unemployed into slavery, in their province
			if (owner != nullptr && slaves != nullptr && !type.is_slave && type.can_be_unemployed && type.strata.get_identifier() == "poor"
				&& owner->get_law_effect(law_effect_t::debt_slavery) > fixed_point_t::_0 && pop->hungry_days >= defines.debt_slavery_hungry_days
				&& pop->get_cash().get_copy_of_value() < pop->get_life_needs_expense() && pop->get_unemployed() > pop_size_t(0)) {
				pop->migration_remainder += owner->get_law_effect(law_effect_t::debt_slavery) * static_cast<std::int32_t>(type_safe::get(pop->get_unemployed()));
				const std::int32_t whole = std::min(take_whole(pop->migration_remainder), std::max(0, type_safe::get(pop->get_size()) - 1));
				if (whole > 0) {
					if (Pop* target = find_or_create_pop(province, *slaves, pop->culture, *pop, pop_deps)) {
						blend_literacy(*target, *pop, whole);
						pop->change_size(-whole, 1);
						target->change_size(whole, 1);
						promoted_total += whole;
					}
				}
			}

			// ---- State atheism (the laws): the clergy becomes clerks
			if (owner != nullptr && &type == clergymen && clerks != nullptr && owner->get_law_effect(law_effect_t::clergy_to_clerks) > fixed_point_t::_0) {
				pop->migration_remainder += owner->get_law_effect(law_effect_t::clergy_to_clerks) * static_cast<std::int32_t>(type_safe::get(pop->get_size()));
				const std::int32_t whole = std::min(take_whole(pop->migration_remainder), type_safe::get(pop->get_size()));
				if (whole > 0) {
					if (Pop* target = find_or_create_pop(province, *clerks, pop->culture, *pop, pop_deps)) {
						blend_literacy(*target, *pop, whole);
						pop->change_size(-whole, 1);
						target->change_size(whole, 1);
						promoted_total += whole;
					}
				}
			}

			// ---- The surplus: the unemployed of a pop that was hungry for a month. A small share of them emigrates (first inside the country
			// to the places with free jobs, abroad only when the whole country has none), the rest dies
			if (pop->hungry_days >= defines.hunger_days_before_death && type.can_be_unemployed && !type.is_slave && owner != nullptr) {
				const std::int32_t surplus = type_safe::get(pop->get_unemployed());
				if (surplus > 0 && surplus >= defines.starvation_min_surplus) {
					const std::size_t own_slot = type_safe::get(owner->index) * type_count + type_safe::get(type.index);
					// TODO the technologies raise the share of the emigrants (the research system adds a bonus here)
					// A pop never disappears from hunger or emigration: at least one person stays
					const std::int32_t people_that_may_go = std::max(0, type_safe::get(pop->get_size()) - 1);
					// Closed borders (the laws): nobody leaves the country, the hungry die
					const std::int32_t emigrants = owner->get_law_effect(law_effect_t::no_emigration) > fixed_point_t::_0 ? 0 : std::min(
						static_cast<std::int32_t>(defines.emigration_share.of_int(surplus)), people_that_may_go
					);
					std::int32_t placed = 0;
					if (emigrants > 0) {
						if (free_jobs_total[own_slot] > 0) {
							placed = move_to_vacancies(*pop, province, own_slot, emigrants, 3);
							emigrated_internal_total += placed;
						} else {
							// Abroad: the country with the most free jobs for this kind of people; a member of the same customs union counts and
							// takes SPHERE_CUSTOMS_MIGRATION_PCT more (version 93)
							const std::int64_t union_pct = 100 + std::max(0, instance_manager.definition_manager.get_define_manager()
								.get_openvic_defines().diplomacy.sphere_customs_migration_pct);
							std::size_t best_slot = own_slot;
							std::int64_t best_free = 0;
							bool best_in_union = false;
							for (std::size_t country = 0; country < country_count; ++country) {
								const std::size_t slot = country * type_count + type_safe::get(type.index);
								CountryInstance const& destination =
									instance_manager.get_country_instance_manager().get_country_instance_by_index(country_index_t(country));
								const bool in_union = owner->shares_sphere_of_kind(destination, sphere_kind_t::CUSTOMS_UNION);
								const std::int64_t weighted = in_union ? free_jobs_total[slot] * union_pct / 100 : free_jobs_total[slot];
								// (a country that lets nobody in takes no emigrants)
								if (slot != own_slot && free_jobs_total[slot] > 0 && weighted > best_free
									&& destination.get_law_effect(law_effect_t::no_immigration) <= fixed_point_t::_0) {
									best_free = weighted;
									best_slot = slot;
									best_in_union = in_union;
								}
							}
							if (best_slot != own_slot) {
								const std::int32_t going = best_in_union ? static_cast<std::int32_t>(std::min<std::int64_t>(
									static_cast<std::int64_t>(emigrants) * union_pct / 100, people_that_may_go
								)) : emigrants;
								placed = move_to_vacancies(*pop, province, best_slot, going, 4);
								emigrated_external_total += placed;
							}
						}
					}
					// The rest of the surplus dies (the emigrants that found no place stay and are counted with the rest)
					const std::int32_t staying = surplus - placed;
					const std::int32_t dying = std::min(
						static_cast<std::int32_t>(defines.surplus_death_share.of_int(std::max(0, staying - (emigrants - placed)))),
						std::max(0, people_that_may_go - placed)
					);
					if (dying > 0) {
						pop->change_size(-dying, 5);
						starved_total += dying;
					}
				}
			}
			pop->hungry_days = 0;

			// ---- Assimilation of unaccepted cultures into the primary culture
			if (owner != nullptr && owner->get_primary_culture() != nullptr
				&& pop->get_culture_status() == Pop::culture_status_t::UNACCEPTED && !type.is_slave) {
				Culture const& primary = *owner->get_primary_culture();
				// Inside a culture group the assimilation is faster
				const std::int32_t multiplier = &pop->culture.group == &primary.group ? defines.assimilation_same_group_multiplier : 1;
				// The citizenship of the laws: faster (cultural exclusion) or none (multiculturalism)
				pop->assimilation_remainder += defines.assimilation_per_month.of(
					fixed_point_t(type_safe::get(pop->get_size())) * multiplier
				) * std::max(fixed_point_t::_0, fixed_point_t::_1 + owner->get_law_effect(law_effect_t::assimilation));
				const std::int32_t whole = std::min(take_whole(pop->assimilation_remainder), type_safe::get(pop->get_size()));
				if (whole > 0) {
					if (Pop* target = find_or_create_pop(province, type, primary, *pop, pop_deps)) {
						blend_literacy(*target, *pop, whole);
						pop->change_size(-whole, 3);
						target->change_size(whole, 3);
						assimilated_total += whole;
					}
				}
			}

			// ---- The slaves of a country that banned the slavery (the laws) become free: farmers, or labourers where the RGO employs them
			// (colonial slavery: only the slaves outside the colonies)
			const fixed_point_t free_slaves = owner == nullptr ? fixed_point_t::_0 : owner->get_law_effect(law_effect_t::free_slaves)
				+ (province.is_colonial_province() ? fixed_point_t::_0 : owner->get_law_effect(law_effect_t::free_slaves_home));
			if (type.is_slave && owner != nullptr && farmers != nullptr && labourers != nullptr && free_slaves > fixed_point_t::_0) {
				PopType const* free_type = farmers;
				if (ProductionType const* const rgo_type = province.get_rgo().get_production_type_nullable(); rgo_type != nullptr) {
					fixed_point_t most = fixed_point_t::_0;
					for (Job const& job : rgo_type->get_jobs()) {
						PopType const* const candidate = pop_manager.get_pop_type_by_index(job.pop_type_index);
						if ((candidate == farmers || candidate == labourers) && job.amount > most) {
							free_type = candidate;
							most = job.amount;
						}
					}
				}
				pop->migration_remainder += free_slaves * static_cast<std::int32_t>(type_safe::get(pop->get_size()));
				const std::int32_t whole = std::min(take_whole(pop->migration_remainder), type_safe::get(pop->get_size()));
				if (whole > 0) {
					if (Pop* target = find_or_create_pop(province, *free_type, pop->culture, *pop, pop_deps)) {
						blend_literacy(*target, *pop, whole);
						pop->change_size(-whole, 1);
						target->change_size(whole, 1);
					}
				}
			}

			// ---- Demotion of unemployed craftsmen / promotion of unemployed farmers and labourers
			const fixed_point_t unemployment = pop->get_unemployment_fraction();
			if (craftsmen != nullptr && farmers != nullptr && labourers != nullptr) {
				if (&type == craftsmen && unemployment > defines.craftsmen_demotion_unemployment.fixed()) {
					PopType const* target_type = labourers;
					// The RGO decides which of the two is needed: the type with more people in the province
					fixed_point_t farmers_size = fixed_point_t::_0, labourers_size = fixed_point_t::_0;
					for (Pop& other : province.get_mutable_pops()) {
						if (&other.get_type() == farmers) {
							farmers_size += type_safe::get(other.get_size());
						} else if (&other.get_type() == labourers) {
							labourers_size += type_safe::get(other.get_size());
						}
					}
					if (farmers_size > labourers_size) {
						target_type = farmers;
					}
					pop->migration_remainder += defines.promotion_per_month.of(fixed_point_t(type_safe::get(pop->get_unemployed())));
					const std::int32_t whole = std::min(take_whole(pop->migration_remainder), type_safe::get(pop->get_size()));
					if (whole > 0) {
						if (Pop* target = find_or_create_pop(province, *target_type, pop->culture, *pop, pop_deps)) {
							blend_literacy(*target, *pop, whole);
							pop->change_size(-whole, 2);
							target->change_size(whole, 2);
						}
					}
				} else if ((&type == farmers || &type == labourers) && state != nullptr && vacancies[state] > 0
					&& (unemployment > defines.peasants_promotion_unemployment.fixed()
						|| (defines.factory_job_promotion_share.numerator > 0
							&& (!factory_pay[state].has_value() || *factory_pay[state] >= own_income)))) {
					// The unemployed as before, and the employed when the factories of the state pay at least what the RGO pays
					if (unemployment > defines.peasants_promotion_unemployment.fixed()) {
						pop->migration_remainder += defines.promotion_per_month.of(fixed_point_t(type_safe::get(pop->get_unemployed())));
					}
					if (defines.factory_job_promotion_share.numerator > 0 && (!factory_pay[state].has_value() || *factory_pay[state] >= own_income)) {
						pop->migration_remainder += defines.factory_job_promotion_share.of(
							fixed_point_t(type_safe::get(pop->get_size()) - type_safe::get(pop->get_unemployed()))
						);
					}
					const std::int32_t whole = std::min({
						take_whole(pop->migration_remainder), type_safe::get(pop->get_size()), vacancies[state]
					});
					if (whole > 0) {
						if (Pop* target = find_or_create_pop(province, *craftsmen, pop->culture, *pop, pop_deps)) {
							blend_literacy(*target, *pop, whole);
							pop->change_size(-whole, 1);
							target->change_size(whole, 1);
							vacancies[state] -= whole;
							promoted_total += whole;
						}
					}
				}
			}

			// ---- The hungry artisans take the free jobs of the factories of the state (craftsmen), else of the RGO of the province
			if (artisans != nullptr && &type == artisans && defines.artisan_demotion_share.numerator > 0
				&& pop->get_life_needs_fulfilled() < defines.artisan_demotion_life_needs.fixed()) {
				PopType const* target_type = nullptr;
				std::int32_t room = 0;
				bool to_factory = false;
				if (craftsmen != nullptr && state != nullptr && vacancies[state] > 0) {
					target_type = craftsmen;
					room = vacancies[state];
					to_factory = true;
				} else {
					ResourceGatheringOperation const& rgo = province.get_rgo();
					ProductionType const* const rgo_type = rgo.get_production_type_nullable();
					if (rgo_type != nullptr) {
						room = type_safe::get(rgo.get_max_employee_count_cache()) - type_safe::get(rgo.get_total_employees_count_cache())
							- rgo_jobs_taken[&province];
						fixed_point_t most = fixed_point_t::_0;
						for (Job const& job : rgo_type->get_jobs()) {
							PopType const* const candidate = pop_manager.get_pop_type_by_index(job.pop_type_index);
							if (candidate != nullptr && candidate->can_be_unemployed && !candidate->is_slave && job.amount > most) {
								target_type = candidate;
								most = job.amount;
							}
						}
					}
				}
				if (target_type != nullptr && room > 0) {
					pop->migration_remainder += defines.artisan_demotion_share.of(fixed_point_t(type_safe::get(pop->get_size())));
					const std::int32_t whole = std::min({ take_whole(pop->migration_remainder), type_safe::get(pop->get_size()), room });
					if (whole > 0) {
						if (Pop* target = find_or_create_pop(province, *target_type, pop->culture, *pop, pop_deps)) {
							blend_literacy(*target, *pop, whole);
							pop->change_size(-whole, 2);
							target->change_size(whole, 2);
							if (to_factory) {
								vacancies[state] -= whole;
							} else {
								rgo_jobs_taken[&province] += whole;
							}
						}
					}
				}
			}

			// ---- Job migration: the unemployed (and a few of the employed when the place pays much more) go to a free job of the region or the next region
			if (job_place_province != nullptr && rgo_worker_types.contains(&type) && pop->get_rebel_type() == nullptr) {
				RgoPlace& place = rgo_places[job_place_province];
				const std::int32_t unemployed_now = type_safe::get(pop->get_unemployed());
				const bool pays_more = place.income_per_worker > fixed_point_t::_0
					&& place.income_per_worker >= own_income * defines.job_migration_profit_ratio;
				const std::int32_t room = std::min(
					place.free_jobs, std::max<std::int32_t>(1, static_cast<std::int32_t>(defines.immigration_fill_share.of_int(place.free_jobs)))
				);
				const std::int32_t take = PopDynamics::job_migration_take(
					defines, unemployed_now, type_safe::get(pop->get_size()) - unemployed_now, pays_more, type_safe::get(pop->get_size()), room
				);
				if (take > 0 && place.job_type != nullptr) {
					if (Pop* target = find_or_create_pop(*job_place_province, *place.job_type, pop->culture, *pop, pop_deps)) {
						blend_literacy(*target, *pop, take);
						pop->change_size(-take, 3);
						target->change_size(take, 3);
						place.free_jobs -= take;
						job_migrated_total += take;
						job_retrained_total += place.job_type != &type ? take : 0;
					}
				}
			}

			// ---- Internal migration to a better province: the unemployed and the hungry leave
			if (best_destination != nullptr && !type.is_slave && type_safe::get(pop->get_size()) > 0
				&& (unemployment > defines.migration_unemployment_threshold.fixed()
					|| pop->get_life_needs_fulfilled() < defines.migration_life_needs_threshold.fixed())) {
				pop->migration_remainder += defines.migration_per_month.of(fixed_point_t(type_safe::get(pop->get_size())));
				const std::int32_t whole = std::min(take_whole(pop->migration_remainder), type_safe::get(pop->get_size()));
				if (whole > 0) {
					if (Pop* target = find_or_create_pop(*best_destination, type, pop->culture, *pop, pop_deps)) {
						blend_literacy(*target, *pop, whole);
						pop->change_size(-whole, 3);
						target->change_size(whole, 3);
						migrated_total += whole;
					}
				}
			}
		}
	}

	SPDLOG_INFO(
		"Population changes: grown {}, migrated {}, assimilated {}, promoted to craftsmen {}, died of hunger {}, emigrated inside the country {} and abroad {}, to free jobs {} (changed the type {})",
		grown_total, migrated_total, assimilated_total, promoted_total, starved_total, emigrated_internal_total, emigrated_external_total, job_migrated_total, job_retrained_total
	);
	pop_manager.record_job_migration(job_migrated_total, job_retrained_total);
}
