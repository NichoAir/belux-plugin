#include "pch.h"
#include "BeluxSync.hpp"

#include <boost/algorithm/string.hpp>

using namespace EuroScopePlugIn;

void BeluxSync::add(const std::string& callsign, const std::string& newScratch, const std::string& oldScratch)
{
	std::string trimmedNew = boost::trim_copy(newScratch);
	std::string trimmedOld = boost::trim_copy(oldScratch);

	auto& q = queue_[callsign];

	// don't queue the same message twice in a row
	if (!q.empty() && boost::iequals(q.back().newScratch, trimmedNew)) return;

	q.push_back({ std::move(trimmedNew), std::move(trimmedOld) });
	states_.try_emplace(callsign);
}

void BeluxSync::addBurst(const std::string& callsign, std::vector<std::string> writes, const std::string& oldScratch)
{
	if (writes.empty()) return;

	Msg msg;
	msg.oldScratch = boost::trim_copy(oldScratch);
	msg.burst = std::move(writes);

	queue_[callsign].push_back(std::move(msg));
	states_.try_emplace(callsign);
}

void BeluxSync::processQueue(CPlugIn* plugin)
{
	const auto now = std::chrono::steady_clock::now();
	if (now - lastRun_ < std::chrono::milliseconds(SLEEP_MS)) return;
	lastRun_ = now;

	if (queue_.empty()) return;

	for (auto it = queue_.begin(); it != queue_.end();)
	{
		const std::string& callsign = it->first;
		auto& q = it->second;
		auto& data = states_[callsign];

		if (q.empty() && data.state == State::Free)
		{
			states_.erase(callsign);
			it = queue_.erase(it);
			continue;
		}

		CFlightPlan fp = plugin->FlightPlanSelect(callsign.c_str());
		if (!fp.IsValid())
		{
			states_.erase(callsign);
			it = queue_.erase(it);
			continue;
		}

		if (data.state == State::Free && !q.empty() && !q.front().burst.empty())
		{
			// fire and forget: write all strings in a row, then restore the old scratch pad
			const Msg burst = std::move(q.front());
			q.pop_front();

			auto assigned = fp.GetControllerAssignedData();
			for (const std::string& text : burst.burst) assigned.SetScratchPadString(text.c_str());
			assigned.SetScratchPadString(burst.oldScratch.c_str());
		}
		else if (data.state == State::Free && !q.empty())
		{
			const std::string toSend = q.front().newScratch;

			data.state = State::WaitOnSync; // state first, ES may call back synchronously
			data.lastTrigger = now;
			fp.GetControllerAssignedData().SetScratchPadString(toSend.c_str());
		}
		else if (data.state != State::Free)
		{
			const double elapsed = std::chrono::duration<double>(now - data.lastTrigger).count();
			if (elapsed >= TIMEOUT_SECONDS)
			{
				// watchdog: echo never arrived, drop the message and restore the old scratch pad
				std::string restore;
				data.state = State::Free;

				if (!q.empty())
				{
					restore = q.front().oldScratch;
					q.pop_front();
				}

				fp.GetControllerAssignedData().SetScratchPadString(restore.c_str());
			}
		}

		++it;
	}
}

void BeluxSync::update(CFlightPlan& flightPlan)
{
	const std::string callsign = flightPlan.GetCallsign();

	auto qIt = queue_.find(callsign);
	auto sIt = states_.find(callsign);
	if (qIt == queue_.end() || sIt == states_.end() || qIt->second.empty()) return;

	auto& q = qIt->second;
	auto& data = sIt->second;

	auto assigned = flightPlan.GetControllerAssignedData();
	const std::string current = boost::trim_copy(std::string(assigned.GetScratchPadString()));

	if (data.state == State::WaitOnSync)
	{
		if (!boost::iequals(current, q.front().newScratch)) return;

		if (boost::iequals(current, q.front().oldScratch))
		{
			q.pop_front();
			data.state = State::Free;
		}
		else
		{
			const std::string restore = q.front().oldScratch; // copy, a re-entrant update may pop the front
			data.state = State::WaitOnRestore;
			data.lastTrigger = std::chrono::steady_clock::now();
			assigned.SetScratchPadString(restore.c_str());
		}
	}
	else if (data.state == State::WaitOnRestore)
	{
		if (boost::iequals(current, q.front().oldScratch))
		{
			q.pop_front();
			data.state = State::Free;
		}
	}
}

void BeluxSync::remove(const std::string& callsign)
{
	queue_.erase(callsign);
	states_.erase(callsign);
}

void BeluxSync::clear()
{
	queue_.clear();
	states_.clear();
}
