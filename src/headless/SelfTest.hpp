#pragma once

namespace OpenVic {
	struct Bookmark;
	struct GameManager;

	/* Self tests of the OpenVic additions, run by the headless executable:
	 *   openvic-simulation.headless --selftest-pure                (no game files needed)
	 *   openvic-simulation.headless -b <Victoria 2> --selftest     (pure tests, then the tests on a real game)
	 * One line per check: "TEST PASS <name>" or "TEST FAIL <name>: <what>", then "SELFTEST SUMMARY passed=N failed=M".
	 * The exit code is not 0 if anything failed. */
	bool run_pure_selftests();
	bool run_data_selftests(GameManager& game_manager, Bookmark const& bookmark);
	/* The game of the determinism test (the orders of the first day, 40 days) played `runs` times in one process: every run is compared day by day
	 * with the first one (a line DETERMINISM per run; the first day that differs and the sums of that day). The rare divergences need many runs. */
	bool run_determinism_loop(GameManager& game_manager, Bookmark const& bookmark, int runs);
}
