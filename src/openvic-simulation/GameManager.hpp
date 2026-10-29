#pragma once

#include <optional>
#include <string_view>

#include <function2/function2.hpp>

#include "openvic-simulation/core/memory/Vector.hpp"
#include "openvic-simulation/dataloader/Dataloader.hpp"
#include "openvic-simulation/dataloader/ModManager.hpp"
#include "openvic-simulation/DefinitionManager.hpp"
#include "openvic-simulation/gen/commit_info.gen.hpp"
#include "openvic-simulation/InstanceManager.hpp"
#include "openvic-simulation/misc/GameRulesManager.hpp"

namespace OpenVic {
	struct GameManager {
		using elapsed_time_getter_func_t = fu2::function_base<true, true, fu2::capacity_none, false, false, uint64_t() const>;

	private:
		static elapsed_time_getter_func_t get_elapsed_usec_time_callback;
		static elapsed_time_getter_func_t get_elapsed_msec_time_callback;

		GameRulesManager PROPERTY(game_rules_manager);
		Dataloader PROPERTY(dataloader);
		DefinitionManager PROPERTY(definition_manager);
		ModManager PROPERTY(mod_manager);
		std::optional<InstanceManager> instance_manager;
		fs::path builtin_data_path;

		InstanceManager::gamestate_updated_func_t gamestate_updated_callback;
		bool PROPERTY_CUSTOM_PREFIX(definitions_loaded, are);
		bool PROPERTY_CUSTOM_PREFIX(mod_descriptors_loaded, are);

	public:
		GameManager(
			InstanceManager::gamestate_updated_func_t new_gamestate_updated_callback,
			elapsed_time_getter_func_t new_get_elapsed_usec_callback,
			elapsed_time_getter_func_t new_get_elapsed_msec_callback
		);
		~GameManager();

		inline constexpr InstanceManager* get_instance_manager() {
			return instance_manager ? &*instance_manager : nullptr;
		}

		inline constexpr InstanceManager const* get_instance_manager() const {
			return instance_manager ? &*instance_manager : nullptr;
		}

		// The folder with the data of OpenVic itself (ov_data: the weapons, their goods and recipes...). It is a root of the dataloader
		// between the base game and the mods, so a mod overrides it with files of the same path. Set it before set_base_path; if it is
		// not set (or is not a folder) the game loads without it, with a warning.
		void set_builtin_data_path(fs::path const& path);
		[[nodiscard]] fs::path const& get_builtin_data_path() const {
			return builtin_data_path;
		}

		bool set_base_path(Dataloader::path_span_t base_path);

		bool load_mod_descriptors();

		bool load_mods(memory::vector<memory::string> const& mods_to_find);

		bool load_definitions(Dataloader::localisation_callback_t localisation_callback);

		bool setup_instance(Bookmark const& bookmark);
		bool is_game_instance_setup() const;
		bool is_bookmark_loaded() const;

		bool start_game_session();
		bool end_game_session();
		bool is_game_session_active() const;

		bool update_clock();
		bool is_advance_due() const {
			return instance_manager && instance_manager->is_advance_due();
		}

		static constexpr std::string_view get_commit_hash() {
			return SIM_COMMIT_HASH;
		}

		static constexpr uint64_t get_commit_timestamp() {
			return SIM_COMMIT_TIMESTAMP;
		}

		static uint64_t get_elapsed_microseconds();
		static uint64_t get_elapsed_milliseconds();
	};
}
