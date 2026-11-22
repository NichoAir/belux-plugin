#pragma once
#include <chrono>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

#include "EuroScopePlugIn.h"

/**
 * Scratch pad based synchronisation between controllers (same mechanism as vSID's SyncManager).
 *
 * A message is "broadcast" by writing it to the scratch pad of a flight plan. ES distributes the scratch pad
 * to all other controllers, which read it in OnFlightPlanControllerAssignedDataUpdate. Afterwards the old
 * scratch pad is restored, so nothing stays on the strip. Messages are queued per callsign and handled one
 * at a time (state machine Free -> WaitOnSync -> WaitOnRestore -> Free) with a watchdog.
 */
class BeluxSync
{
public:
	/** Queue a message. oldScratch is restored after the message was seen. */
	void add(const std::string& callsign, const std::string& newScratch, const std::string& oldScratch);
	/**
	 * Queues several scratch pad strings that are written in a row, followed by oldScratch (no echo is awaited).
	 * For ES ground states / clearance keywords: ES consumes them instead of keeping them in the scratch pad,
	 * so there is nothing to wait for (same way the "Sync" plugin does it).
	 */
	void addBurst(const std::string& callsign, std::vector<std::string> writes, const std::string& oldScratch);
	/** Starts the next queued message / runs the watchdog. Cheap, rate limited internally. */
	void processQueue(EuroScopePlugIn::CPlugIn* plugin);
	/** Call from OnFlightPlanControllerAssignedDataUpdate (scratch pad changes). */
	void update(EuroScopePlugIn::CFlightPlan& flightPlan);
	/** Forget everything about a callsign (disconnect). */
	void remove(const std::string& callsign);
	void clear();

private:
	enum class State { Free, WaitOnSync, WaitOnRestore };

	struct Msg
	{
		std::string newScratch;
		std::string oldScratch;
		std::vector<std::string> burst; // non-empty: fire and forget, newScratch unused
	};

	struct Data
	{
		State state = State::Free;
		std::chrono::steady_clock::time_point lastTrigger{};
	};

	static constexpr double TIMEOUT_SECONDS = 0.5;
	static constexpr int SLEEP_MS = 150;

	std::unordered_map<std::string, std::deque<Msg>> queue_;
	std::unordered_map<std::string, Data> states_;
	std::chrono::steady_clock::time_point lastRun_{};
};
