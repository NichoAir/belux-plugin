#include "pch.h"
#include "BeluxPlugin.hpp"

#include <execution>
#include <string>
#include <map>
#include <set>
#include <utility>
#include <iomanip>
#include <Windows.h>
#include <cstdio>
#include <optional>
#include <algorithm>
#include <cstring>
#include <Psapi.h>

#pragma comment(lib, "Psapi.lib")

using namespace std;
using namespace EuroScopePlugIn;

namespace
{
	constexpr const char* REQ_TAG = ".BELUX_REQ_";
	constexpr const char* AUTO_TAG = ".BELUX_AUTO_";
	constexpr const char* INT_TAG = ".BELUX_INT_";

	struct RequestKind
	{
		const char* label; // popup list entry
		const char* type; // internal name / sync name
		char letter; // tag item prefix
	};

	constexpr RequestKind REQUEST_KINDS[] = {
		{ "IFR Clearance", "clearance", 'C' },
		{ "Startup", "startup", 'S' },
		{ "Pushback", "pushback", 'P' },
		{ "Taxi", "taxi", 'T' },
		{ "Departure", "departure", 'D' },
		{ "VFR", "vfr", 'V' },
	};

	const RequestKind* requestKindByLabel(const string& label)
	{
		for (const auto& kind : REQUEST_KINDS) if (label == kind.label) return &kind;
		return nullptr;
	}

	const RequestKind* requestKindByType(const string& type)
	{
		for (const auto& kind : REQUEST_KINDS) if (type == kind.type) return &kind;
		return nullptr;
	}

	struct StatusName
	{
		const char* name;
		BeluxGroundStatus status;
	};

	// accepted spellings: ES ground states (NSTS, STUP, ...) and GroundRadar scratch pad strings (ST-UP, DE-ICE, ...)
	constexpr StatusName STATUS_NAMES[] = {
		{ "NOSTATE", BeluxGroundStatus::NoState }, { "NSTS", BeluxGroundStatus::NoState },
		{ "ONFREQ", BeluxGroundStatus::OnFreq },
		{ "DE-ICE", BeluxGroundStatus::Stby },
		{ "ST-UP", BeluxGroundStatus::Stup }, { "STUP", BeluxGroundStatus::Stup },
		{ "PUSH", BeluxGroundStatus::Push },
		{ "TAXI", BeluxGroundStatus::Taxi },
		{ "LINEUP", BeluxGroundStatus::Lineup },
		{ "DEPA", BeluxGroundStatus::Takeoff },
		{ "TXIN", BeluxGroundStatus::TaxiIn },
		{ "PARK", BeluxGroundStatus::Parked },
	};

	std::optional<BeluxGroundStatus> statusFromString(const string& value)
	{
		for (const auto& entry : STATUS_NAMES) if (value == entry.name) return entry.status;
		return std::nullopt;
	}

	/** The string written to the scratch pad so that ES / GroundRadar of the other controllers pick the status up. */
	const char* statusToScratch(BeluxGroundStatus status)
	{
		switch (status)
		{
		case BeluxGroundStatus::NoState: return "NOSTATE";
		case BeluxGroundStatus::OnFreq: return "ONFREQ";
		case BeluxGroundStatus::Stby: return "DE-ICE";
		case BeluxGroundStatus::Stup: return "ST-UP";
		case BeluxGroundStatus::Push: return "PUSH";
		case BeluxGroundStatus::Taxi: return "TAXI";
		case BeluxGroundStatus::Lineup: return "LINEUP";
		case BeluxGroundStatus::Takeoff: return "DEPA";
		case BeluxGroundStatus::TaxiIn: return "TXIN";
		case BeluxGroundStatus::Parked: return "PARK";
		}
		return "NOSTATE";
	}

	string upperCopy(string s)
	{
		std::transform(s.begin(), s.end(), s.begin(), [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
		return s;
	}

	/** Returns the value following tag in the (upper case) scratch pad, up to the next space. */
	std::optional<string> tagValue(const string& scratch, const char* tag)
	{
		const size_t pos = scratch.find(tag);
		if (pos == string::npos) return std::nullopt;

		string value = scratch.substr(pos + strlen(tag));
		const size_t space = value.find(' ');
		if (space != string::npos) value.erase(space);
		return boost::trim_copy(value);
	}
}

using boost::asio::ip::tcp;

#ifdef _DEBUG
bool DEBUG_print = true;
#else
bool DEBUG_print = false;
#endif

// Time (in seconds) before we request new information about this flight from the API.
constexpr int DATA_RETENTION_LENGTH = 60;

BeluxPlugin::BeluxPlugin(void) : CPlugIn(EuroScopePlugIn::COMPATIBILITY_CODE, MY_PLUGIN_NAME, MY_PLUGIN_VERSION,
                                         MY_PLUGIN_DEVELOPER, MY_PLUGIN_COPYRIGHT)
{
	versionCheck();
	loadJSONconfig();

	procedureAssigner = new ProcedureAssigner([this](const std::string& text)
	{
		printDebugMessage("SID", text);
	});

	getActiveRunways();
	procedureAssigner->set_runways(&activeDepRunways, &activeArrRunways);
	if (procedureAssigner->setup_lara() < 1)
	{
		printMessage("SID", "Could not parse LARA");
	} else
	{
		DisplayUserMessage("Belux Plugin", "SID", "Areas loaded", true, true, false, false, false);
	}
	processed = new set<string>();

	// Register Tag item(s).
	RegisterTagItemType("Assigned Gate", TagDefinitions::item_gate_assign);
	RegisterTagItemFunction("refresh assigned gate", TagDefinitions::function_gate_refresh);
	RegisterTagItemType("Mach number", TagDefinitions::item_mach_number);
	RegisterTagItemFunction("Assign RWY/SID", TagDefinitions::function_force_sid);
	RegisterTagItemType("Procedure Suggestion", TagDefinitions::item_proc_suggestion);
	RegisterTagItemType("VSpeed", TagDefinitions::vspeed);
	RegisterTagItemType("Intersection (able)", TagDefinitions::item_intersection_able);
	RegisterTagItemType("Intersection (cleared)", TagDefinitions::item_intersection_cleared);
	RegisterTagItemFunction("Set runway intersection", TagDefinitions::function_intersection_set);
	RegisterTagItemFunction("Select runway intersection as able", TagDefinitions::function_intersection_able);
	RegisterTagItemType("Request", TagDefinitions::item_request);
	RegisterTagItemType("Request Timer", TagDefinitions::item_request_timer);
	RegisterTagItemFunction("Request Menu", TagDefinitions::function_request_menu);

	ProcessMETAR("EBLG", GetAirportInfo("EBLG"));
	if (function_fetch_gates)
		FetchAndProcessGates();

	if (function_mach_visualisation)
	{
		printDebugMessage("API", "fetched weather file " + utils.fetch_weather_file());
	}
	try
	{
		const auto parsed = procedureAssigner->fetch_sid_allocation();
		printDebugMessage("SID", "Parsed SID allocations: " + std::to_string(parsed));
		if (parsed < 1)
		{
			printMessage("SID", "Could not parse SID allocation file.");
		}
	}
	catch (exception _e)
	{
		printDebugMessage("SID", "Caught an exception");
	}

	detectExternalPlugins();
	refreshControllers();

	if (function_check_runway_and_sid)
	{
		// auto mode needs a controller connection and the lowest-station check, so it is applied from OnTimer
		autoStartupPending = true;
		printMessage("Functionalities", "Auto mode is requested by the config and will start once you are connected as a controller (only if you are the lowest station). Turn it off with .belux auto");
	}
}

BeluxPlugin::~BeluxPlugin()
{
	for (auto* screen : screens) screen->detach();
	screens.clear();
	delete processed;
}

void BeluxPlugin::versionCheck()
{
	string loadingMessage = MY_PLUGIN_VERSION;
	loadingMessage += " loaded.";
	DisplayUserMessage("Message", "Belux Plugin", loadingMessage.c_str(), true, true, true, false, false);
	DisplayUserMessage("Belux Plugin", "Plugin version", loadingMessage.c_str(), true, true, true, false, false);

	if (std::string(MY_PLUGIN_VERSION).find('-') == std::string::npos)
	{
		const string latest_version = GetLatestPluginVersion();
		if (latest_version != "S_ERR")
		{
			if (latest_version != MY_PLUGIN_VERSION)
			{
				string message = "You are using an older version of the Belux plugin. Updating to " + latest_version +
					" is adviced. For safety, API-based functions have been disabled";
				function_fetch_gates = false;
				MessageBox(0, message.c_str(), "Belux plugin version", MB_OK | MB_ICONQUESTION);
			}
			else
			{
				DisplayUserMessage("Belux Plugin", "Plugin version", "You are using the latest version", true, true,
				                   true, false, false);
			}
		}
		else
		{
			DisplayUserMessage("Belux Plugin", "Plugin version", "Failed verifying latest version", true, true, true,
			                   false, false);
		}
	}
	else
	{
		DisplayUserMessage("Belux Plugin", "Plugin version", "using BETA version --> skiped verifying version", true,
		                   true, true, false, false);
	}
}

void BeluxPlugin::loadJSONconfig()
{
	// Getting the DLL file folder
	char DllPathFile[_MAX_PATH];
	string DllPath;

	GetModuleFileNameA(HINSTANCE(&__ImageBase), DllPathFile, sizeof(DllPathFile));
	DllPath = DllPathFile;
	DllPath.resize(DllPath.size() - strlen("belux.dll"));
	string configPath = DllPath + "\\belux_config.json";

	stringstream ss;
	ifstream ifs(configPath.c_str(), std::ios::binary);
	if (ifs.is_open())
	{
		ss << ifs.rdbuf();
		ifs.close();
		printDebugMessage("config", "Loading belux JSON config... ");
		Document document;
		if (document.Parse<0>(ss.str().c_str()).HasParseError())
		{
			AfxMessageBox(
				"An error parsing Belux configuration occurred.\nOnce fixed, reload the config by typing '.belux reload'",
				MB_OK);
		}
		if (document.HasMember("debug_mode"))
		{
			DEBUG_print = document["debug_mode"].GetBool();
			printDebugMessage("config", "debug mode " + string(DEBUG_print ? "enabled" : "disabled"));
		}
		if (document.HasMember("API_timeout"))
		{
			timeout_value = document["API_timeout"].GetInt();
			printDebugMessage("config", "API timeout to " + std::to_string(timeout_value));
		}
		if (document.HasMember("on_gate_change"))
		{
			// TODO: now why don't you work from the start....
			const string ogc = document["on_gate_change"].GetString();
			if (ogc == "blink")
			{
				on_gate_change = 2;
				printDebugMessage("config", "Alert on gate change: on");
			}
			else if (ogc == "quiet")
			{
				on_gate_change = 1;
				printDebugMessage("config", "Alert on gate change: quietly");
			}
			else if (ogc == "none")
			{
				on_gate_change = 0;
				printDebugMessage("config", "Alert on gate change: off");
			}
			else
			{
				printDebugMessage("config", "Alert on gate change: invalid (default to on)");
			}
		}
		else { printDebugMessage("config", "No alert on gate change config."); }
		if (document.HasMember("functionalities"))
		{
			if (document["functionalities"].GetObject().HasMember("fetch_gates"))
			{
				function_fetch_gates = document["functionalities"].GetObject()["fetch_gates"].GetBool();
				printDebugMessage("config", "Gate assigner " + string(function_fetch_gates ? "enabled" : "disabled"));
			}
			if (document["functionalities"].GetObject().HasMember("set_initial_climb"))
			{
				function_set_initial_climb = document["functionalities"].GetObject()["set_initial_climb"].GetBool();
				printDebugMessage(
					"config", "Initial climb " + string(function_set_initial_climb ? "enabled" : "disabled"));
			}
			if (document["functionalities"].GetObject().HasMember("mach_visualisation"))
			{
				function_mach_visualisation = document["functionalities"].GetObject()["mach_visualisation"].GetBool();
				printDebugMessage(
					"config", "Mach visualisation " + string(function_mach_visualisation ? "enabled" : "disabled"));
			}
			if (document["functionalities"].GetObject().HasMember("rwy_sid_assigner"))
			{
				function_check_runway_and_sid = document["functionalities"].GetObject()["rwy_sid_assigner"].GetBool();
				printDebugMessage(
					"config", "rwy/sid assigner " + string(function_check_runway_and_sid ? "enabled" : "disabled"));
			}
		}
		if (document.HasMember("vspeed"))
		{
			if (document["vspeed"].GetObject().HasMember("threshold") && document["vspeed"].GetObject()["threshold"].GetType() == Type::kNumberType)
			{
				vspeed_threshold = document["vspeed"].GetObject()["threshold"].GetUint();
			}
		}
		if (document.HasMember("requests") && document["requests"].IsObject())
		{
			// minutes after which the request timer turns yellow / red
			const rapidjson::Value& cfg = document["requests"];
			if (cfg.HasMember("caution_minutes") && cfg["caution_minutes"].IsInt()) request_caution_minutes = cfg["caution_minutes"].GetInt();
			if (cfg.HasMember("warning_minutes") && cfg["warning_minutes"].IsInt()) request_warning_minutes = cfg["warning_minutes"].GetInt();
		}
		intersectionList.clear();
		if (document.HasMember("intersection") && document["intersection"].IsObject())
		{
			const rapidjson::Value& cfg = document["intersection"];

			const auto readColor = [](const rapidjson::Value& v, COLORREF& out)
			{
				if (v.IsArray() && v.Size() == 3 && v[0].IsInt() && v[1].IsInt() && v[2].IsInt())
					out = RGB(v[0].GetInt(), v[1].GetInt(), v[2].GetInt());
			};

			if (cfg.HasMember("able_color")) readColor(cfg["able_color"], intersectionAbleColor);
			if (cfg.HasMember("set_color")) readColor(cfg["set_color"], intersectionSetColor);

			// "list": { "EBBR": { "25R": ["A1", "B2"], "19": [...] } }
			if (cfg.HasMember("list") && cfg["list"].IsObject())
			{
				for (const auto& apt : cfg["list"].GetObject())
				{
					if (!apt.value.IsObject()) continue;

					for (const auto& rwy : apt.value.GetObject())
					{
						if (!rwy.value.IsArray()) continue;

						for (const auto& entry : rwy.value.GetArray())
						{
							if (entry.IsString()) intersectionList[apt.name.GetString()][rwy.name.GetString()].push_back(entry.GetString());
						}
					}
				}
			}
		}
	}
}

void BeluxPlugin::OnFlightPlanFlightPlanDataUpdate(CFlightPlan FlightPlan)
{
}

void BeluxPlugin::ProcessFlightPlans()
{
	for (EuroScopePlugIn::CRadarTarget rt = this->RadarTargetSelectFirst(); rt.IsValid(); rt = this->
	     RadarTargetSelectNext(rt))
	{
		EuroScopePlugIn::CRadarTargetPositionData pos = rt.GetPosition();
		EuroScopePlugIn::CFlightPlan fp = rt.GetCorrelatedFlightPlan();

		if (!fp.IsValid()) continue;

		string dep_airport = fp.GetFlightPlanData().GetOrigin();
		string callsign = fp.GetCallsign();

		if (force_new_procedure)
		{
			procedureAssigner->process_flight_plan(fp, true);
		}
		else if (isAutoActive(dep_airport) && isIfr(fp)) // VFR aircraft are never handled by auto mode
		{
			// auto mode: SID/RWY (existing suggestion logic) + CCAMS squawk + sync marker
			autoProcess(fp, dep_airport);
		}

		if (!activeAirports.contains(dep_airport)
			|| !fp.IsValid() || !fp.GetCorrelatedRadarTarget().IsValid()
			// OR flightplan has not been loaded/correleted correctly?
			|| processed->contains(callsign)
			|| (strcmp(fp.GetTrackingControllerId(), "") != 0 && !fp.GetTrackingControllerIsMe())
			// OR aircraft is tracked (with exception of aircraft tracked by current controller)
			|| fp.GetCorrelatedRadarTarget().GetGS() > 5 // OR moving: Ground speed > 5knots
			|| fp.GetCorrelatedRadarTarget().GetPosition().GetPressureAltitude() > 1500
			// OR flying: Altitude > 1500 feet
			|| fp.GetCorrelatedRadarTarget().GetPosition().GetPressureAltitude() == 0)
		{
			// OR altitude == 0 -> uncorrelated? alitude should never be zero
			continue; // THEN SKIP 
		}
		processed->insert(callsign);

		if (function_set_initial_climb)
		{
			//Saftey check: only set CFL once
			int CFL = 0;
			if (dep_airport == "EBBR" || dep_airport == "EBOS" || dep_airport == "EBLG")
			{
				if (QNH[dep_airport] == 0)
				{
					ProcessMETAR(dep_airport, GetAirportInfo(dep_airport));
				}

				if (QNH[dep_airport] > 995)
				{
					CFL = 6000;
				}
				else if (QNH[dep_airport] > 959)
				{
					CFL = 7000;
				}
				else
				{
					CFL = 8000;
				}
			}
			else if (dep_airport == "ELLX" || dep_airport == "EBCI")
			{
				CFL = 4000;
			}
			else if (dep_airport == "EBAW" || dep_airport == "EBKT")
			{
				CFL = 3000;
			}

			if (CFL > 0 && fp.GetFlightPlanData().GetFinalAltitude() > CFL && fp.GetControllerAssignedData().
				GetClearedAltitude() != CFL)
			{
				fp.GetControllerAssignedData().SetClearedAltitude(CFL);
			}
		}
	}
	force_new_procedure = false;
}

void BeluxPlugin::OnFlightPlanDisconnect(CFlightPlan FlightPlan)
{
	{
		const string cs = FlightPlan.GetCallsign();
		requests.erase(cs);
		groundTrack.erase(cs);
		squawkRequested.erase(cs);
		sync.remove(cs);
		intersections.erase(cs);
		squawkQueue.erase(std::remove(squawkQueue.begin(), squawkQueue.end(), cs), squawkQueue.end());
	}
	if (function_set_initial_climb)
	{
		processed->erase(FlightPlan.GetCallsign());
	}
	procedureAssigner->on_disconnect(FlightPlan);
}

void BeluxPlugin::OnNewMetarReceived(const char* sStation, const char* sFullMetar)
{
	if (function_set_initial_climb)
	{
		ProcessMETAR(sStation, sFullMetar);
	}
}

void BeluxPlugin::ProcessMETAR(string airport, string metar)
{
    /*
    This does make us save too many METARs, but let's not get too bothered with that, it's a few extra bytes.
    */
	try {
		const size_t pos = metar.find("Q") + 1;
		QNH[airport] = stoi(metar.substr(pos, 4));
		printDebugMessage("CFL", "SET " + airport + " Q" + to_string(QNH[airport]));
	}
	catch (const exception& e) {}
}

void BeluxPlugin::OnFunctionCall(int FunctionId, const char* sItemString, POINT Pt, RECT Area)
{
	switch (FunctionId)
	{
	case TagDefinitions::function_gate_refresh:
		if (function_fetch_gates)
			FetchAndProcessGates();
		break;
	case TagDefinitions::function_force_sid:
		if (!procedureAssigner->process_flight_plan(FlightPlanSelectASEL(), true))
		{
			printMessage("SID", "Failed to assign requested SID/RWY");
		}
		break;
	case TagDefinitions::function_request_menu:
		openRequestMenu(FlightPlanSelectASEL(), Area);
		break;
	case TagDefinitions::function_request_pick:
		{
			CFlightPlan fp = FlightPlanSelectASEL();
			if (fp.IsValid()) applyRequest(fp, sItemString);
		}
		break;
	case TagDefinitions::function_intersection_set:
		openIntersectionMenu(FlightPlanSelectASEL(), Area, true);
		break;
	case TagDefinitions::function_intersection_able:
		openIntersectionMenu(FlightPlanSelectASEL(), Area, false);
		break;
	case TagDefinitions::function_intersection_set_pick:
	case TagDefinitions::function_intersection_able_pick:
		{
			// popup list entry or result of the "Custom" edit field
			const bool set = (FunctionId == TagDefinitions::function_intersection_set_pick);
			const string pick = sItemString;
			CFlightPlan fp = FlightPlanSelectASEL();

			if (!fp.IsValid() || pick.empty() || pick == "NO RWY" || pick == "NO INTS") break;

			if (pick == "Custom")
			{
				OpenPopupEdit(Area, FunctionId, "");
				break;
			}

			applyIntersection(fp, pick == "Clear" ? string() : pick, set);
		}
		break;
	}
}

void BeluxPlugin::OnTimer(int Counter)
{
	sync.processQueue(this);

	if (!ccamsLoaded && Counter % 30 == 0) detectExternalPlugins(); // CCAMS may be loaded after us

	refreshControllers();
	applyStartupAuto();
	enforceLowestStation();
	processSquawkQueue();

	if (function_fetch_gates && (Counter % DATA_RETENTION_LENGTH == 0))
	{
		FetchAndProcessGates();
	}

	if (Counter % 5 == 0)
	{
		ProcessFlightPlans();
	}

	if (function_mach_visualisation && (Counter % (30 * 60) == 0))
	{
		printDebugMessage("API", "fetched weather file " + utils.fetch_weather_file());
	}

	/*
	 * Every ten seconds, retry LARA, only if 0 entries are loaded
	 */
	if ((Counter % 10) == 0 && procedureAssigner->area_rules_count() < 1)
	{
	if (procedureAssigner->setup_lara() < 1)
	{
		printMessage("SID", "Could not parse LARA");
	} else
	{
		DisplayUserMessage("Belux Plugin", "SID", "Areas loaded", true, true, false, false, false);
	}
	}
}

void BeluxPlugin::OnAirportRunwayActivityChanged(void)
{
	getActiveRunways();
	procedureAssigner->set_runways(&activeDepRunways, &activeArrRunways);
	procedureAssigner->reprocess_all();
}

void BeluxPlugin::FetchAndProcessGates()
{
	gatePlanner.parse_json(GetGateInfo());
	for (auto iter = gatePlanner.gate_list.begin(); iter != gatePlanner.gate_list.end(); ++iter)
	{
		string cs = iter->first;
		BeluxGateEntry entry = iter->second;
		EuroScopePlugIn::CFlightPlan fp = FlightPlanSelect(cs.c_str());
		string dest = fp.GetFlightPlanData().GetDestination();
		string gate = gatePlanner.gate_list[cs].gate;

		if (gatePlanner.gate_list[cs].gate_has_changed)
		{
			//---GATE Change detected------
			string message = cs + " ==> " + gatePlanner.gate_list[cs].gate;
			if (on_gate_change)
			{
				const bool blink = on_gate_change > 1;
				DisplayUserMessage("Belux Plugin", "GATE CHANGE", message.c_str(),
				                   blink, blink, blink, blink, blink);
			}
			gatePlanner.gate_list[cs].color = RGB(50, 205, 50);
			fp.GetControllerAssignedData().SetFlightStripAnnotation(4, gate.c_str());
		}

		if (string(fp.GetControllerAssignedData().GetFlightStripAnnotation(4)) != gate)
		{
			fp.GetControllerAssignedData().SetFlightStripAnnotation(4, gate.c_str());
			//FlightPlan.GetControllerAssignedData().SetScratchPadString(gatePlanner.gate_list[cs].gate.c_str());
		}
	}
}

void BeluxPlugin::OnGetTagItem(CFlightPlan FlightPlan, CRadarTarget RadarTarget,
                               int ItemCode, int TagData,
                               char sItemString[16], int* pColorCode,
                               COLORREF* pRGB, double* pFontSize)
{
	const size_t buf_size = 16;
	// Only work on tag items we actually care about.
	sync.processQueue(this);

	switch (ItemCode)
	{
	case TagDefinitions::item_intersection_able:
	case TagDefinitions::item_intersection_cleared:
		{
			if (!FlightPlan.IsValid()) break;

			// two independent items: yellow = aircraft is able to use the intersection, green = cleared to it
			const bool cleared = (ItemCode == TagDefinitions::item_intersection_cleared);
			const auto entry = intersections.find(FlightPlan.GetCallsign());
			const string value = (entry == intersections.end()) ? string() : (cleared ? entry->second.second : entry->second.first);

			if (!value.empty())
			{
				(*pColorCode) = EuroScopePlugIn::TAG_COLOR_RGB_DEFINED;
				(*pRGB) = cleared ? intersectionSetColor : intersectionAbleColor;
				strncpy_s(sItemString, buf_size, value.c_str(), buf_size - 1);
			}
			else if (activeAirports.contains(string(FlightPlan.GetFlightPlanData().GetOrigin())) && RadarTarget.GetGS() < 50)
			{
				// placeholder so the item stays clickable on the ground
				(*pColorCode) = EuroScopePlugIn::TAG_COLOR_RGB_DEFINED;
				(*pRGB) = RGB(150, 150, 150);
				strcpy_s(sItemString, buf_size, "-");
			}
		}
		break;

	case TagDefinitions::item_request:
	case TagDefinitions::item_request_timer:
		{
			if (!FlightPlan.IsValid()) break;

			const string reqCallsign = FlightPlan.GetCallsign();
			const auto req = requests.find(reqCallsign);

			if (req == requests.end())
			{
				if (ItemCode == TagDefinitions::item_request &&
					activeAirports.contains(string(FlightPlan.GetFlightPlanData().GetOrigin())) && RadarTarget.GetGS() < 50)
				{
					// placeholder so the item stays clickable on the ground
					(*pColorCode) = EuroScopePlugIn::TAG_COLOR_RGB_DEFINED;
					(*pRGB) = RGB(150, 150, 150);
					strcpy_s(sItemString, buf_size, "-");
				}
				break;
			}

			(*pColorCode) = EuroScopePlugIn::TAG_COLOR_RGB_DEFINED;

			if (ItemCode == TagDefinitions::item_request)
			{
				// request letter + place in the queue, e.g. S1 = first in the startup queue
				(*pRGB) = RGB(255, 255, 255);
				const string text = string(1, req->second.letter) + std::to_string(requestPosition(reqCallsign));
				strcpy_s(sItemString, buf_size, text.c_str());
			}
			else
			{
				const long long nowSec = chrono::duration_cast<chrono::seconds>(chrono::system_clock::now().time_since_epoch()).count();
				const int minutes = static_cast<int>((nowSec - req->second.time) / 60);

				if (minutes >= request_warning_minutes) (*pRGB) = RGB(255, 70, 70);
				else if (minutes >= request_caution_minutes) (*pRGB) = RGB(255, 220, 0);
				else (*pRGB) = RGB(190, 190, 190);

				const string text = std::to_string(minutes) + "m";
				strcpy_s(sItemString, buf_size, text.c_str());
			}
		}
		break;

	case TagDefinitions::vspeed:
		{
			const auto pos = RadarTarget.GetPosition();
			const auto prev = RadarTarget.GetPreviousPosition(pos);
			const auto delta = pos.GetPressureAltitude() - prev.GetPressureAltitude();
			const auto dt = prev.GetReceivedTime() - pos.GetReceivedTime();
			const auto val = dt != 0 ? delta * 60 / dt : 0;
			if (abs(val) >= vspeed_threshold)
			{
				// yes yes no rounding idc
				// width set to 3, because the sign is counted as a digit...
				snprintf(sItemString, buf_size, "%+03d", val / 100);
			}
		}
		break;
	case TagDefinitions::item_mach_number:
		if (!function_mach_visualisation)
			break;

		{
			const string cs = FlightPlan.GetCallsign();
			const int gs = FlightPlan.GetCorrelatedRadarTarget().GetGS();
			const int FL = (int)(FlightPlan.GetCorrelatedRadarTarget().GetPosition().GetFlightLevel() / 100);
			const double hdg = FlightPlan.GetCorrelatedRadarTarget().GetTrackHeading();
			const double lat = FlightPlan.GetCorrelatedRadarTarget().GetPosition().GetPosition().m_Latitude;
			const double lon = FlightPlan.GetCorrelatedRadarTarget().GetPosition().GetPosition().m_Longitude;
			if (gs > 0)
			{
				const auto result = utils.calculate_mach(cs, FL, gs, hdg, lat, lon);
				//printDebugMessage(to_string(gs) + " " + to_string(FL) + " " + to_string(hdg) + " " + to_string(lat) + " " + to_string(lon) + "--> " + to_string(mach));
				const double mach = get<0>(result);
				const int ias = (int)get<1>(result);
				std::stringstream stream;
				stream << std::fixed << std::setprecision(3) << mach;
				string output;
				if (FL < 245)
				{
					output = "/" + to_string(ias);
				}
				else
				{
					output = stream.str().substr(1) + "/" + to_string(ias);
				}
				strcpy_s(sItemString, 16, output.c_str());
			}
		}
		break;

	case TagDefinitions::item_gate_assign:
		if (!function_fetch_gates)
			break;
		{
			const string cs = FlightPlan.GetCallsign();
			if (gatePlanner.gate_list.contains(cs))
			{
				if (gatePlanner.gate_list[cs].color != NULL)
				{
					(*pColorCode) = EuroScopePlugIn::TAG_COLOR_RGB_DEFINED;
					(*pRGB) = gatePlanner.gate_list[cs].color;
				}

				const string gateItem = gatePlanner.gate_list[cs].gate + (gatePlanner.gate_list[cs].suggest25R
					                                                          ? "*"
					                                                          : "");
				strncpy_s(sItemString,buf_size, gateItem.c_str(), buf_size - 1);
			}
		}
		break;

	case TagDefinitions::item_proc_suggestion:
		{
			const auto maybe_suggestion = procedureAssigner->suggest(FlightPlan, true);

			if (!maybe_suggestion.has_value())
			{
				strcpy_s(sItemString, 16, "?"); // Indicate an error case
				break;
			}

			const auto suggestion = maybe_suggestion.value();

			const string route = FlightPlan.GetFlightPlanData().GetRoute();
			if (suggestion.rwy != string(FlightPlan.GetFlightPlanData().GetDepartureRwy())
				|| string(FlightPlan.GetFlightPlanData().GetSidName()) != suggestion.sid)
			{
				(*pColorCode) = EuroScopePlugIn::TAG_COLOR_INFORMATION;
			}
			else
			{
				(*pColorCode) = EuroScopePlugIn::TAG_COLOR_NON_CONCERNED;
			}


			const string text = suggestion.sid + "/" + suggestion.rwy;
			strncpy_s(sItemString, buf_size, text.c_str(), buf_size - 1);
		}
		break;
	}
}

string BeluxPlugin::GetAirportInfo(const string& airport)
{
	const string host = "metar.vatsim.net";
	const string uri = "/search_metar.php?id=" + airport;

	// Form the request.
	std::stringstream request;
	request << "GET " << uri << " HTTP/1.1\r\n";;
	request << "Host: " << host << "\r\n\r\n";

	string response = GetHttpsRequest(host, uri, request.str(), false);
	return response;
}

void BeluxPlugin::printDebugMessage(const string& function, const string& message)
{
	if (DEBUG_print)
		DisplayUserMessage(string("Belux Plugin DEBUG - " + function).c_str(), "DEBUG", message.c_str(), true, true,
		                   true, true, true);
}

void BeluxPlugin::printMessage(const string& topic, const string& message)
{
	DisplayUserMessage("Belux Plugin", topic.c_str(), message.c_str(), true, true, true, true, true);
}

string BeluxPlugin::GetLatestPluginVersion()
{
	const string host = "api.beluxvacc.org";
	const string uri = "/belux-gate-manager-api/version/plugin";

	// Form the request.
	std::stringstream request;
	request << "GET " << uri << " HTTP/1.1\r\n";;
	request << "Host: " << host << "\r\n\r\n";

	return GetHttpsRequest(host, uri, request.str(), false);
}

std::string BeluxPlugin::GetGateInfo()
{
	const string host = "api.beluxvacc.org";
	const string uri = "/belux-gate-manager-api/get_all_assigned_gates/";

	// Form the request.
	std::stringstream request;
	request << "GET " << uri << " HTTP/1.1\r\n";;
	request << "Host: " << host << "\r\n\r\n";

	string response = GetHttpsRequest(host, uri, request.str(), true);
	return response;
}

void BeluxPlugin::getActiveRunways()
{
	map<string, vector<string>> active_dep_runways;
	map<string, vector<string>> active_arr_runways;
	set<string> active_airports;

	for (CSectorElement ad = SectorFileElementSelectFirst(SECTOR_ELEMENT_AIRPORT); ad.IsValid(); ad =
	     SectorFileElementSelectNext(ad, SECTOR_ELEMENT_AIRPORT))
	{
		if (ad.IsElementActive(false, 0) && ad.IsElementActive(true, 0))
		{
			active_airports.emplace(ad.GetName());
		}
	}

	// Auto load the airport config on ASR opened.
	for (CSectorElement rwy = SectorFileElementSelectFirst(SECTOR_ELEMENT_RUNWAY);
	     rwy.IsValid();
	     rwy = SectorFileElementSelectNext(rwy, SECTOR_ELEMENT_RUNWAY))
	{
		const auto ad_name = boost::trim_copy(string(rwy.GetAirportName()));
		if (!active_airports.contains(ad_name))
		{
			continue; // Inactive AD
		}

		if (rwy.IsElementActive(true, 0))
		{
			active_dep_runways[ad_name].push_back(rwy.GetRunwayName(0));
		}
		if (rwy.IsElementActive(false, 0))
		{
			active_arr_runways[ad_name].push_back(rwy.GetRunwayName(0));
		}

		if (rwy.IsElementActive(true, 1))
		{
			active_dep_runways[ad_name].push_back(rwy.GetRunwayName(1));
		}
		if (rwy.IsElementActive(false, 1))
		{
			active_arr_runways[ad_name].push_back(rwy.GetRunwayName(1));
		}
	}

	if (DEBUG_print)
	{
		for (const auto& dep_config : active_dep_runways)
		{
			printDebugMessage(
				"RWY", "Dep " + dep_config.first + ": " + reduce(dep_config.second.begin(), dep_config.second.end(),
				                                                 string("")));
		}
		for (const auto& arr_config : active_arr_runways)
		{
			printDebugMessage(
				"RWY", "Arr " + arr_config.first + ": " + reduce(arr_config.second.begin(), arr_config.second.end(),
				                                                 string("")));
		}
	}

	activeAirports = active_airports;
	activeArrRunways = active_arr_runways;
	activeDepRunways = active_dep_runways;
}

string BeluxPlugin::SwapGate(string callsign, string gate)
{
	const string host = "api.beluxvacc.org";
	const string uri = "/belux-gate-manager-api/swap_gate/";

	// Form the request.
	std::stringstream request;
	request << "POST " << uri << " HTTP/1.1\r\n";;
	request << "Host: " << host << "\r\n";
	request << "Content-Type: application/x-www-form-urlencoded\r\n";
	request << "Authorization: " AUTH_SECRET "\r\n";
	request << "Content-Length: " << (18 + callsign.length() + gate.length()) << "\r\n\r\n";
	request << "callsign=" + callsign + "&gate_id=" + gate << "\r\n";

	string response = GetHttpsRequest(host, uri, request.str(), false);
	return response;
}

string BeluxPlugin::GetHttpsRequest(string host, string uri, string request_string, bool expect_long_json)
{
	string data = "";
	try
	{
		// Initialize the asio service.
		boost::asio::io_context io_service; // io_service was removed in Boost 1.87
		boost::asio::ssl::context context(boost::asio::ssl::context::sslv23);
		boost::asio::ssl::stream<tcp::socket> ssock(io_service, context);
		if (!SSL_set_tlsext_host_name(ssock.native_handle(), host.c_str()))
		{
			boost::system::error_code ec{static_cast<int>(::ERR_get_error()), boost::asio::error::get_ssl_category()};
			throw boost::system::system_error{ec};
		}

		// Get a list of endpoints corresponding to the server name.
		tcp::resolver resolver(io_service);
		auto it = resolver.resolve(host, "https"); // resolver::query was removed in Boost 1.87
		boost::asio::connect(ssock.lowest_layer(), it);
		ssock.lowest_layer().set_option(
			boost::asio::detail::socket_option::integer<SOL_SOCKET, SO_RCVTIMEO>{timeout_value});
		ssock.lowest_layer().set_option(
			boost::asio::detail::socket_option::integer<SOL_SOCKET, SO_SNDTIMEO>{timeout_value});

		// Do the SSL handshake
		ssock.handshake(boost::asio::ssl::stream_base::handshake_type::client);

		// Send the request.
		boost::asio::streambuf request;
		ostream request_stream(&request);
		request_stream << request_string;
		boost::asio::write(ssock, request);

		// Read the response line.
		boost::asio::streambuf response;
		response.prepare(4 * 1024 * 1024); //4MB
		boost::asio::read_until(ssock, response, "\r\n");

		// Check that response is OK.
		istream response_stream(&response);
		string http_version;
		response_stream >> http_version;
		unsigned int status_code;
		response_stream >> status_code;
		string status_message;
		getline(response_stream, status_message);
		if (!response_stream || http_version.substr(0, 5) != "HTTP/" || status_code != 200)
		{
			throw exception("HTTPS status: " + status_code);
		}

		// Read the response headers, which are terminated by a blank line.
		boost::asio::read_until(ssock, response, "\r\n\r\n");
		string header;
		while (getline(response_stream, header) && header != "\r")
			continue;

		// Write whatever content we already have to output.
		ostringstream stream;
		if (response.size() > 0)
		{
			stream << &response;
		}

		if (stream.str().empty()) {
			return stream.str();
		}

		if (expect_long_json && stream.str().back() != ']')
		{
			// Read until EOF, writing data to output as we go.
			boost::system::error_code error;
			while (boost::asio::read(ssock, response,
			                         boost::asio::transfer_at_least(1), error))
			{
				stream << &response;
				if (stream.str().back() == ']')
					break;
			}
		}
		data = stream.str();
		string dbgmsg = "received: " + data;
		printDebugMessage("API", dbgmsg);

		return data;
	}
	catch (exception& e)
	{
		DisplayUserMessage("Belux Plugin", "HTTPS error", e.what(), true, true, true, false, false);
		return "HTTPS_ERROR";
	}
}

bool BeluxPlugin::OnCompileCommand(const char* sCommandLine)
{
	string buffer{sCommandLine};
	if (boost::algorithm::starts_with(sCommandLine, ".belux help"))
	{
		printMessage("-", "Belux CLI");
		printMessage("-", ".belux reload                          - reload json config");
		printMessage("-", ".belux force-sid                       - force fresh SID/RWY assignment");
		printMessage("-", ".belux fresh-sid                       - Clear SID cache");
		printMessage("-", ".belux auto [ICAO ...]                 - toggle auto mode (SID/RWY + CCAMS squawk), lowest station only");
		printMessage("-", ".belux sync                           - re-send clearance flags, ground states, requests, intersections of the named airports (.belux sync EBBR)");
		printMessage("-", ".belux gate(on/off)                    - enable/disable gate assignment");
		printMessage("-", ".belux climb(on/off)                   - enable/disable initial climb assignment");
		printMessage("-", ".belux mach(on/off)                    - enable/disable mach visualisation");
		printMessage("-", ".belux timeout <integer>               - set the API timeout value");
		printMessage("-", ".belux refreshgates                    - refresh gates from the API");
		printMessage("-", ".belux setgate <gate>                  - assign gate to selected aircraft");
		printMessage("-", ".belux alert_gates <blink|quiet|none>  - toggle flashing on gate reassignment");
		return true;
	}


	if (boost::algorithm::starts_with(sCommandLine, ".belux auto"))
	{
		handleAutoCommand(buffer);
		return true;
	}

	if (boost::algorithm::starts_with(sCommandLine, ".belux sync"))
	{
		vector<string> tokens;
		boost::split(tokens, buffer, boost::is_any_of(" "), boost::token_compress_on);

		set<string> airports; // tokens[0] = ".belux", tokens[1] = "sync", rest = airports
		for (size_t i = 2; i < tokens.size(); ++i)
		{
			if (!tokens[i].empty()) airports.insert(upperCopy(tokens[i]));
		}

		if (airports.empty())
		{
			// never sync automatically for all airports
			printMessage("Sync", "Please name the airport(s) to sync, e.g. .belux sync EBBR");
			return true;
		}

		resyncStates(airports);
		return true;
	}
	if (boost::algorithm::starts_with(sCommandLine, ".belux reload"))
	{
		loadJSONconfig();
		printMessage("Config", "reloaded JSON config file...");
		return true;
	}

	if (boost::algorithm::starts_with(sCommandLine, ".belux pfp"))
	{
		ProcessFlightPlans();
		return true;
	}

	if (boost::algorithm::starts_with(sCommandLine, ".belux setgate") || boost::algorithm::starts_with(
		sCommandLine, ".bsg"))
	{
		if (ControllerMyself().GetFacility() >= 3 || DEBUG_print)
		{
			bool longcmd = boost::algorithm::starts_with(sCommandLine, ".belux setgate");
			string gate = (longcmd ? buffer.erase(0, 15) : buffer.erase(0, 4));

			string selected_callsign = RadarTargetSelectASEL().GetCallsign();
			if (selected_callsign != "" && gate != "")
			{
				string result = SwapGate(selected_callsign, gate);
				string message;
				if (result != "HTTPS_ERROR")
				{
					message = selected_callsign + " succesfully assigned to gate " + gate;
					gatePlanner.gate_list[selected_callsign].gate = gate;
					gatePlanner.gate_list[selected_callsign].isFetched = true;

					EuroScopePlugIn::CFlightPlan fp = FlightPlanSelect(selected_callsign.c_str());
					fp.GetControllerAssignedData().SetFlightStripAnnotation(4, gate.c_str());
				}
				else
				{
					message = "Something went wrong when trying to assign gate " + gate + "  to " + selected_callsign;
				}
				printMessage("Gate assignment", message);
				return true;
			}
		}
		return false;
	}
	if (boost::algorithm::starts_with(sCommandLine, ".belux gateon") || boost::algorithm::starts_with(
		sCommandLine, ".belux gateoff"))
	{
		function_fetch_gates = (buffer.erase(0, 11) == "on");
		printMessage("Functionalities", "Gate assignment " + string(function_fetch_gates ? "enabled" : "disabled"));
		return true;
	}

	if (boost::algorithm::starts_with(sCommandLine, ".belux climbon") || boost::algorithm::starts_with(
		sCommandLine, ".belux climboff"))
	{
		function_set_initial_climb = (buffer.erase(0, 12) == "on");
		printMessage("Functionalities",
		             "Initial climb assignement " + string(function_set_initial_climb ? "enabled" : "disabled"));
		return true;
	}

	if (boost::algorithm::starts_with(sCommandLine, ".belux machon") || boost::algorithm::starts_with(
		sCommandLine, ".belux machoff"))
	{
		function_mach_visualisation = (buffer.erase(0, 11) == "on");
		printMessage("Functionalities",
		             "Mach visualisation " + string(function_mach_visualisation ? "enabled" : "disabled"));
		if (function_mach_visualisation)
			printDebugMessage("API", "fetched weather file " + utils.fetch_weather_file());
		return true;
	}
	if (boost::algorithm::starts_with(sCommandLine, ".belux machupdate"))
	{
		printDebugMessage("API", "fetched weather file " + utils.fetch_weather_file());
		return true;
	}
	if (boost::algorithm::starts_with(sCommandLine, ".belux rwysidon") || boost::algorithm::starts_with(
		sCommandLine, ".belux rwysidoff"))
	{
		function_check_runway_and_sid = (buffer.erase(0, 13) == "on");
		autoStartupPending = false;
		if (function_check_runway_and_sid)
		{
			enableAutoAll(); // same as .belux auto for all airports, honours the lowest-station rule
		}
		else
		{
			autoAirports.clear();
			printMessage("Functionalities", "Auto mode disabled for all airports");
		}
		return true;
	}

	if (boost::algorithm::starts_with(sCommandLine, ".belux debugon") || boost::algorithm::starts_with(
			sCommandLine, ".dbgon")
		|| boost::algorithm::starts_with(sCommandLine, ".belux debugoff") || boost::algorithm::starts_with(
			sCommandLine, ".dbgoff"))
	{
		DEBUG_print = (buffer == ".dbgon" || buffer.erase(0, 12) == "on");
		printMessage("Functionalities", "Debug mode " + string(DEBUG_print ? "enabled" : "disabled"));
		return true;
	}

	if (boost::algorithm::starts_with(sCommandLine, ".belux timeout"))
	{
		string buffer{sCommandLine};
		buffer.erase(0, 15);
		try
		{
			timeout_value = stoi(buffer);
			printMessage("Functionalities", "Set timeout to " + std::to_string(timeout_value));
			return true;
		}
		catch (exception& e)
		{
			return false;
		}
	}
	if (boost::algorithm::starts_with(sCommandLine, ".belux refreshgates") || boost::algorithm::starts_with(
		sCommandLine, ".rf"))
	{
		if (function_fetch_gates)
			FetchAndProcessGates();
		return true;
	}

	if (boost::algorithm::starts_with(sCommandLine, ".belux force-sid"))
	{
		procedureAssigner->reprocess_all();
		force_new_procedure = true;
		return true;
	}

	if (boost::algorithm::starts_with(sCommandLine, ".belux fresh-sid"))
	{
		procedureAssigner->reprocess_all();
		return true;
	}

	if (boost::algorithm::starts_with(sCommandLine, ".belux alert_gates"))
	{
		string value = buffer.erase(0, string(".belux alert_gates ").length());
		if (value == "blink")
		{
			on_gate_change = 2;
			printMessage("Functionalities", "Alert on gate change: blink");
		}
		else if (value == "quiet")
		{
			on_gate_change = 1;
			printMessage("Functionalities", "Alert on gate change: quiet");
		}
		else if (value == "none")
		{
			on_gate_change = 0;
			printMessage("Functionalities", "Alert on gate change: none");
		}
		else
		{
			printMessage("Functionalities", "Alert on gate change: invalid (default blink)");
		}

		return true;
	}

	return false;
}


// ===========================================================================
// Auto mode, squawk and sync
// ===========================================================================

bool BeluxPlugin::isMyselfController()
{
	auto me = ControllerMyself();
	return me.IsValid() && me.IsController();
}

void BeluxPlugin::refreshControllers()
{
	map<string, map<string, int>> fresh;

	auto me = ControllerMyself();
	const string myCallsign = me.IsValid() ? string(me.GetCallsign()) : string();

	for (CController c = ControllerSelectFirst(); c.IsValid(); c = ControllerSelectNext(c))
	{
		if (!c.IsController()) continue;

		const string cs = c.GetCallsign();
		if (cs == myCallsign) continue;

		const int facility = c.GetFacility();
		if (facility < 2) continue; // ATIS / observers / FSS don't count (2 DEL, 3 GND, 4 TWR, 5 APP, 6 CTR)

		if (cs.size() < 5 || cs[4] != '_') continue; // expect ICAO_xxx
		fresh[cs.substr(0, 4)][cs] = facility;
	}

	airportControllers = std::move(fresh);
}

/**
 * Same rule as vSID's AirportData::hasLowerAtc, reduced to facility levels:
 * toActivate == false: another station with a LOWER facility is online (running auto mode has to stop)
 * toActivate == true : another station with a lower OR EQUAL facility is online (auto mode can't be started)
 */
bool BeluxPlugin::hasLowerAtc(const string& icao, bool toActivate)
{
	auto me = ControllerMyself();
	if (!me.IsValid()) return true;

	const int myFacility = me.GetFacility();
	const auto it = airportControllers.find(icao);
	if (it == airportControllers.end()) return false;

	for (const auto& [callsign, facility] : it->second)
	{
		if (toActivate ? facility <= myFacility : facility < myFacility) return true;
	}
	return false;
}

bool BeluxPlugin::enableAuto(const string& icao)
{
	if (!activeDepRunways.contains(icao))
	{
		printMessage("Auto", "[" + icao + "] is not an active departure airport.");
		return false;
	}

	if (!isMyselfController())
	{
		printMessage("Auto", "Auto mode can only be used when connected as a controller.");
		return false;
	}

	if (hasLowerAtc(icao, true))
	{
		string list;
		for (const auto& [callsign, facility] : airportControllers[icao])
		{
			if (!list.empty()) list += " | ";
			list += callsign;
		}
		printMessage("Auto", "[" + icao + "] Cannot activate auto mode. Lower or same level controller online: " + list);
		return false;
	}

	autoAirports.insert(icao);
	procedureAssigner->reprocess_all();
	if (!ccamsLoaded)
	{
		printMessage("Auto", "[" + icao + "] CCAMS was not detected, squawks will not be assigned automatically.");
	}
	return true;
}

void BeluxPlugin::enableAutoAll()
{
	string summary;
	for (const auto& [icao, runways] : activeDepRunways)
	{
		if (enableAuto(icao))
		{
			if (!summary.empty()) summary += " | ";
			summary += "[" + icao + "] Automode [ON]";
		}
	}
	if (!summary.empty()) printMessage("Auto", summary);
}

void BeluxPlugin::applyStartupAuto()
{
	if (!autoStartupPending || !isMyselfController()) return;

	autoStartupPending = false;
	enableAutoAll();
}

/** The client lost its connection: only auto mode is switched off. Shared data (requests, intersections, ...) is kept. */
void BeluxPlugin::handleDisconnect()
{
	if (!autoAirports.empty())
	{
		autoAirports.clear();
		printMessage("Auto", "Disconnected. Auto mode disabled.");
	}

	// the config flag starts auto mode again after the next connect, otherwise it has to be enabled manually
	autoStartupPending = function_check_runway_and_sid;
}

/** Runs every second: auto mode stops as soon as a lower station shows up or the airport is no longer active. */
void BeluxPlugin::enforceLowestStation()
{
	const bool connected = (GetConnectionType() != EuroScopePlugIn::CONNECTION_TYPE_NO);

	if (wasConnected && !connected) handleDisconnect();
	wasConnected = connected;

	for (auto it = autoAirports.begin(); it != autoAirports.end();)
	{
		const string icao = *it;

		if (!isMyselfController() || !activeDepRunways.contains(icao))
		{
			printMessage("Auto", "[" + icao + "] Disabling auto mode. Airport is no longer active or you are not a controller.");
			it = autoAirports.erase(it);
		}
		else if (hasLowerAtc(icao, false))
		{
			printMessage("Auto", "[" + icao + "] Disabling auto mode. A lower level controller is online.");
			it = autoAirports.erase(it);
		}
		else
		{
			++it;
		}
	}
}

void BeluxPlugin::handleAutoCommand(const string& commandLine)
{
	vector<string> tokens;
	boost::split(tokens, commandLine, boost::is_any_of(" "), boost::token_compress_on);

	set<string> targets; // tokens[0] = ".belux", tokens[1] = "auto", rest = airports
	for (size_t i = 2; i < tokens.size(); ++i)
	{
		if (!tokens[i].empty()) targets.insert(upperCopy(tokens[i]));
	}

	if (targets.empty())
	{
		for (const auto& [icao, runways] : activeDepRunways) targets.insert(icao);
	}

	if (targets.empty())
	{
		printMessage("Auto", "No active departure airports.");
		return;
	}

	string summary;
	for (const string& icao : targets)
	{
		bool on;
		if (autoAirports.contains(icao))
		{
			autoAirports.erase(icao);
			on = false;
		}
		else if (enableAuto(icao)) on = true;
		else continue; // reason already printed

		if (!summary.empty()) summary += " | ";
		summary += "[" + icao + "] Automode [" + (on ? "ON" : "OFF") + "]";
	}

	if (!summary.empty()) printMessage("Auto", summary);
}

/** Ground + not tracked by someone else + correlated. (Same idea as ProcedureAssigner::should_process.) */
bool BeluxPlugin::isGroundAndMine(CFlightPlan& fp)
{
	if (!fp.IsValid() || !isIfr(fp)) return false; // VFR: no squawk from auto mode

	auto rt = fp.GetCorrelatedRadarTarget();
	if (!rt.IsValid()) return false;

	if (strcmp(fp.GetTrackingControllerId(), "") != 0 && !fp.GetTrackingControllerIsMe()) return false;

	const int alt = rt.GetPosition().GetPressureAltitude();
	if (alt > 1500 || alt == 0) return false;

	return rt.GetGS() <= 50;
}

void BeluxPlugin::autoProcess(CFlightPlan& fp, const string& adep)
{
	const string callsign = fp.GetCallsign();
	const string oldScratch = fp.GetControllerAssignedData().GetScratchPadString();

	// process_flight_plan skips plans with clearance flag, tracked by others, already carrying a SID, etc.
	if (const auto sid = procedureAssigner->process_flight_plan(fp, false); sid.has_value())
	{
		printDebugMessage("AUTO", callsign + " auto assigned " + sid->sid + "/" + sid->rwy);

		// tell the other controllers that auto mode set this (they warn / back off when two stations auto-assign)
		auto me = ControllerMyself();
		if (me.IsValid())
		{
			sync.add(callsign, string(AUTO_TAG) + me.GetCallsign(), oldScratch);
		}
	}

	queueSquawk(fp);
}

/** Another controller announced an auto assignment. */
void BeluxPlugin::onAutoMarker(CFlightPlan& fp, const string& sender)
{
	auto me = ControllerMyself();
	if (!me.IsValid()) return;

	const string myCallsign = me.GetCallsign();
	if (sender.empty() || sender == myCallsign) return; // own echo

	const string adep = fp.GetFlightPlanData().GetOrigin();
	if (!autoAirports.contains(adep)) return;

	printMessage("Auto", string(fp.GetCallsign()) + " was assigned a SID/RWY by auto mode of " + sender);

	// Two stations in auto mode for the same airport: the higher level backs off, on equal level the
	// alphabetically later callsign backs off, so both sides reach the same decision.
	const auto apt = airportControllers.find(adep);
	if (apt == airportControllers.end()) return;
	const auto other = apt->second.find(sender);
	if (other == apt->second.end()) return;

	const int myFacility = me.GetFacility();
	if (other->second < myFacility || (other->second == myFacility && sender < myCallsign))
	{
		autoAirports.erase(adep);
		printMessage("Auto", "[" + adep + "] Disabling auto mode. " + sender + " is already running auto mode.");
	}
}

// ---------------------------------------------------------------------------
// squawk via CCAMS
// ---------------------------------------------------------------------------

void BeluxPlugin::detectExternalPlugins()
{
	HANDLE process = GetCurrentProcess();
	HMODULE modules[1024];
	DWORD needed = 0;

	if (!EnumProcessModules(process, modules, sizeof(modules), &needed)) return;

	size_t count = needed / sizeof(HMODULE);
	if (count > 1024) count = 1024;

	for (size_t i = 0; i < count; ++i)
	{
		char name[MAX_PATH] = { 0 };
		if (!GetModuleFileNameExA(process, modules[i], name, MAX_PATH)) continue;

		string lower = name;
		std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

		if (lower.find("ccams.dll") != string::npos)
		{
			if (!ccamsLoaded) printDebugMessage("AUTO", "CCAMS plugin detected");
			ccamsLoaded = true;
		}
	}
}

void BeluxPlugin::queueSquawk(CFlightPlan& fp)
{
	if (!ccamsLoaded || !isGroundAndMine(fp)) return;

	const string squawk = fp.GetControllerAssignedData().GetSquawk();
	if (!(squawk.empty() || squawk == "0000" || squawk == "1234")) return; // never overwrite an existing squawk

	const string callsign = fp.GetCallsign();
	const auto now = chrono::steady_clock::now();

	// don't spam CCAMS, retry only after a minute if the squawk is still missing
	if (const auto it = squawkRequested.find(callsign); it != squawkRequested.end() && now - it->second < chrono::seconds(60)) return;

	squawkRequested[callsign] = now;
	if (std::find(squawkQueue.begin(), squawkQueue.end(), callsign) == squawkQueue.end())
	{
		squawkQueue.push_back(callsign);
	}
}

/** Runs every second. CCAMS gets one request every 2 seconds (like vSID), one aircraft per request. */
void BeluxPlugin::processSquawkQueue()
{
	if (squawkQueue.empty()) return;

	const auto now = chrono::steady_clock::now();
	if (now - lastSquawkTP < chrono::seconds(2)) return;

	const string callsign = squawkQueue.front();
	squawkQueue.pop_front();

	CFlightPlan fp = FlightPlanSelect(callsign.c_str());
	if (!fp.IsValid() || !isGroundAndMine(fp)) return;

	if (!isAutoActive(fp.GetFlightPlanData().GetOrigin())) return; // auto mode was switched off meanwhile

	const string squawk = fp.GetControllerAssignedData().GetSquawk();
	if (!(squawk.empty() || squawk == "0000" || squawk == "1234")) return; // someone else was faster

	if (callCcamsSquawk(callsign))
	{
		lastSquawkTP = now;
		printDebugMessage("AUTO", callsign + " requested squawk from CCAMS");
	}
}

bool BeluxPlugin::callCcamsSquawk(const string& callsign)
{
	if (screens.empty())
	{
		if (!noScreenWarned)
		{
			printMessage("Auto", "Squawk assignment needs an open radar screen (ASR). No screen found, squawks are not assigned.");
			noScreenWarned = true;
		}
		return false;
	}
	noScreenWarned = false;

	// ES only allows calling other plugins' tag functions via a radar screen. 871 = CCAMS "auto assign squawk".
	// ES selects the aircraft the tag function is started for, so remember the current selection and restore it
	CFlightPlan previousSelection = FlightPlanSelectASEL();

	(*screens.begin())->StartTagFunction(callsign.c_str(), "CCAMS", EuroScopePlugIn::TAG_ITEM_TYPE_CALLSIGN,
	                                     callsign.c_str(), "CCAMS", 871, POINT(), RECT());

	if (previousSelection.IsValid()) SetASELAircraft(previousSelection);
	return true;
}

// ---------------------------------------------------------------------------
// requests + sync receiving
// ---------------------------------------------------------------------------

bool BeluxPlugin::isIfr(CFlightPlan& fp)
{
	if (!fp.IsValid()) return false;

	const string type = fp.GetFlightPlanData().GetPlanType();
	return type == "I" || type == "Y"; // Y = IFR first
}

/** Place in the queue: number of earlier requests of the same type at the same airport, + 1. */
int BeluxPlugin::requestPosition(const string& callsign) const
{
	const auto mine = requests.find(callsign);
	if (mine == requests.end()) return 0;

	int position = 1;
	for (const auto& [other, req] : requests)
	{
		if (other == callsign || req.type != mine->second.type || req.adep != mine->second.adep) continue;

		if (req.time < mine->second.time || (req.time == mine->second.time && other < callsign)) ++position;
	}
	return position;
}

void BeluxPlugin::openRequestMenu(CFlightPlan fp, RECT area)
{
	if (!fp.IsValid()) return;
	if (!activeAirports.contains(string(fp.GetFlightPlanData().GetOrigin()))) return;

	const int pickId = TagDefinitions::function_request_pick;
	const bool vfr = (string(fp.GetFlightPlanData().GetPlanType()) == "V");

	OpenPopupList(area, "REQ", 1);
	AddPopupListElement("No Req", "No Req", pickId, false, EuroScopePlugIn::POPUP_ELEMENT_NO_CHECKBOX, false, false);

	for (const auto& kind : REQUEST_KINDS)
	{
		const string type = kind.type;
		if (type == "clearance" && vfr) continue; // IFR clearance only for IFR
		if (type == "vfr" && !vfr) continue;

		AddPopupListElement(kind.label, kind.label, pickId, false, EuroScopePlugIn::POPUP_ELEMENT_NO_CHECKBOX, false, false);
	}
}

/** Stores the request locally and sends it to the other controllers. "No Req" removes it. */
void BeluxPlugin::applyRequest(CFlightPlan fp, const string& label)
{
	if (!fp.IsValid()) return;

	const string callsign = fp.GetCallsign();
	const string adep = fp.GetFlightPlanData().GetOrigin();
	const string oldScratch = fp.GetControllerAssignedData().GetScratchPadString();

	if (label == "No Req")
	{
		requests.erase(callsign);
		sync.add(callsign, string(REQ_TAG) + "NONE/0", oldScratch);
		return;
	}

	const RequestKind* kind = requestKindByLabel(label);
	if (kind == nullptr) return;

	if (!activeAirports.contains(adep))
	{
		printMessage("Request", "[" + adep + "] is not an active airport, can't set request.");
		return;
	}

	const string type = kind->type;

	long long time = chrono::duration_cast<chrono::seconds>(chrono::system_clock::now().time_since_epoch()).count();

	if (const auto existing = requests.find(callsign); existing != requests.end())
	{
		if (existing->second.type == type) return; // already requested, keep the original time
	}

	requests[callsign] = Request{ type, kind->letter, time, adep };
	sync.add(callsign, string(REQ_TAG) + type + "/" + std::to_string(time), oldScratch);
}

/**
 * Remembers clearance flag and ground status changes. EuroScope keeps the clearance flag and its own ground state,
 * but not the GroundRadar statuses (ONFREQ, DE-ICE, LINEUP, ...), which only arrive as scratch pad strings.
 */
void BeluxPlugin::trackGroundState(CFlightPlan& fp, int dataType)
{
	std::optional<bool> cleared;
	std::optional<BeluxGroundStatus> status;

	switch (dataType)
	{
	case EuroScopePlugIn::CTR_DATA_TYPE_CLEARENCE_FLAG:
		cleared = fp.GetClearenceFlag();
		break;
	case EuroScopePlugIn::CTR_DATA_TYPE_SCRATCH_PAD_STRING:
		status = statusFromString(upperCopy(boost::trim_copy(string(fp.GetControllerAssignedData().GetScratchPadString()))));
		break;
	case EuroScopePlugIn::CTR_DATA_TYPE_GROUND_STATE:
		status = statusFromString(fp.GetGroundState());
		break;
	default:
		return;
	}

	if (!cleared.has_value() && !status.has_value()) return;

	const string callsign = fp.GetCallsign();
	auto& track = groundTrack[callsign];

	if (cleared.has_value()) track.cleared = *cleared;
	if (status.has_value()) track.status = *status;

	// nothing worth remembering
	if (!track.cleared && track.status.value_or(BeluxGroundStatus::NoState) == BeluxGroundStatus::NoState)
	{
		groundTrack.erase(callsign);
	}
}

/**
 * .belux sync <ICAO ...> - replays everything the other controllers might have missed (e.g. after a reconnect):
 * clearance flags and ground statuses of aircraft on the ground (like the "Sync" plugin), plus our requests and intersections.
 */
void BeluxPlugin::resyncStates(const set<string>& airports)
{
	size_t aircraftSynced = 0;
	size_t requestsSent = 0;
	size_t intersectionsSent = 0;

	// requests
	for (const auto& [callsign, req] : requests)
	{
		if (!airports.contains(req.adep)) continue;

		CFlightPlan fp = FlightPlanSelect(callsign.c_str());
		if (!fp.IsValid()) continue;

		sync.add(callsign, string(REQ_TAG) + req.type + "/" + std::to_string(req.time),
		         fp.GetControllerAssignedData().GetScratchPadString());
		++requestsSent;
	}

	// intersections
	for (const auto& [callsign, entry] : intersections)
	{
		CFlightPlan fp = FlightPlanSelect(callsign.c_str());
		if (!fp.IsValid() || !airports.contains(string(fp.GetFlightPlanData().GetOrigin()))) continue;

		const string scratch = fp.GetControllerAssignedData().GetScratchPadString();
		if (!entry.first.empty()) { sync.add(callsign, string(INT_TAG) + "A_" + entry.first, scratch); ++intersectionsSent; }
		if (!entry.second.empty()) { sync.add(callsign, string(INT_TAG) + "S_" + entry.second, scratch); ++intersectionsSent; }
	}

	// clearance flags + ground statuses (same aircraft filter as the "Sync" plugin)
	for (CFlightPlan fp = FlightPlanSelectFirst(); fp.IsValid(); fp = FlightPlanSelectNext(fp))
	{
		if (!airports.contains(string(fp.GetFlightPlanData().GetOrigin()))) continue;
		if (fp.GetDistanceFromOrigin() > 5.0) continue;

		auto rt = fp.GetCorrelatedRadarTarget();
		if (!rt.IsValid() || rt.GetGS() > 60) continue;

		const char* tracking = fp.GetTrackingControllerCallsign();
		if (tracking != nullptr && *tracking != '\0') continue; // someone is tracking it, not on the ground list anymore

		const string callsign = fp.GetCallsign();
		const auto tracked = groundTrack.find(callsign);

		// stored values first, fall back to what ES knows itself (e.g. state set before the plugin was loaded)
		const bool cleared = (tracked != groundTrack.end() && tracked->second.cleared) || fp.GetClearenceFlag();

		std::optional<BeluxGroundStatus> status;
		if (tracked != groundTrack.end()) status = tracked->second.status;
		if (!status.has_value()) status = statusFromString(fp.GetGroundState());
		if (status.has_value() && *status == BeluxGroundStatus::NoState) status.reset();

		std::vector<std::string> writes;
		if (cleared) writes.emplace_back("CLEA");
		if (status.has_value()) writes.emplace_back(statusToScratch(*status));
		if (writes.empty()) continue;

		// don't "restore" a stale status keyword: if the old scratch pad is one, keep the newest value instead
		const string oldScratch = fp.GetControllerAssignedData().GetScratchPadString();
		const string oldUpper = upperCopy(boost::trim_copy(oldScratch));
		const bool oldIsKeyword = statusFromString(oldUpper).has_value() || oldUpper == "CLEA" || oldUpper == "NOTC";

		sync.addBurst(callsign, writes, oldIsKeyword ? writes.back() : oldScratch);
		++aircraftSynced;
	}

	printMessage("Sync", "Synced " + std::to_string(aircraftSynced) + " aircraft (clearance flag / ground status), "
	                         + std::to_string(requestsSent) + " request(s), " + std::to_string(intersectionsSent) + " intersection(s).");
}

// ---------------------------------------------------------------------------
// runway intersections ("holding points")
// ---------------------------------------------------------------------------

/** Popup list with the intersections configured for the departure runway of the selected aircraft. */
void BeluxPlugin::openIntersectionMenu(CFlightPlan fp, RECT area, bool set)
{
	if (!fp.IsValid()) return;
	if (!activeAirports.contains(string(fp.GetFlightPlanData().GetOrigin()))) return;

	const string adep = fp.GetFlightPlanData().GetOrigin();
	const string rwy = fp.GetFlightPlanData().GetDepartureRwy();
	const int pickId = set ? TagDefinitions::function_intersection_set_pick : TagDefinitions::function_intersection_able_pick;

	OpenPopupList(area, set ? "Set Int" : "Able Int", 1);

	bool listed = false;

	if (rwy.empty())
	{
		AddPopupListElement("NO RWY", "NO RWY", pickId, false, EuroScopePlugIn::POPUP_ELEMENT_NO_CHECKBOX, true, false);
		listed = true;
	}
	else if (const auto apt = intersectionList.find(adep); apt != intersectionList.end())
	{
		if (const auto list = apt->second.find(rwy); list != apt->second.end())
		{
			for (const string& name : list->second)
			{
				AddPopupListElement(name.c_str(), name.c_str(), pickId, false, EuroScopePlugIn::POPUP_ELEMENT_NO_CHECKBOX, false, false);
				listed = true;
			}
		}
	}

	if (!listed)
	{
		AddPopupListElement("NO INTS", "NO INTS", pickId, false, EuroScopePlugIn::POPUP_ELEMENT_NO_CHECKBOX, true, false);
	}

	AddPopupListElement("Custom", "Custom", pickId, false, EuroScopePlugIn::POPUP_ELEMENT_NO_CHECKBOX, false, true);
	AddPopupListElement("Clear", "Clear", pickId, false, EuroScopePlugIn::POPUP_ELEMENT_NO_CHECKBOX, false, true);
}

/** Stores the intersection locally and sends it to the other controllers. Empty name clears it. */
void BeluxPlugin::applyIntersection(CFlightPlan fp, const string& rawName, bool set)
{
	if (!fp.IsValid()) return;

	const string callsign = fp.GetCallsign();
	const string name = upperCopy(boost::trim_copy(rawName));

	if (!name.empty() && (name.size() > 5 || !std::all_of(name.begin(), name.end(), [](unsigned char ch) { return std::isalnum(ch) != 0; })))
	{
		printMessage("Intersection", "Invalid intersection [" + name + "]. Use up to 5 letters/digits.");
		return;
	}

	// able (first) and cleared (second) are independent, an empty name clears only the chosen one
	auto& entry = intersections[callsign];
	(set ? entry.second : entry.first) = name;
	if (entry.first.empty() && entry.second.empty()) intersections.erase(callsign);

	const string message = string(set ? "S_" : "A_") + (name.empty() ? "NONE" : name);
	sync.add(callsign, string(INT_TAG) + message, fp.GetControllerAssignedData().GetScratchPadString());
}
void BeluxPlugin::OnFlightPlanControllerAssignedDataUpdate(CFlightPlan FlightPlan, int DataType)
{
	if (!FlightPlan.IsValid()) return;

	trackGroundState(FlightPlan, DataType); // remember clearance flag / ground status for .belux sync

	if (DataType == EuroScopePlugIn::CTR_DATA_TYPE_CLEARENCE_FLAG)
	{
		// IFR clearance given -> the clearance request is done
		if (FlightPlan.GetClearenceFlag())
		{
			const auto pending = requests.find(FlightPlan.GetCallsign());
			if (pending != requests.end() && pending->second.type == "clearance") requests.erase(pending);
		}
		return;
	}

	if (DataType == EuroScopePlugIn::CTR_DATA_TYPE_GROUND_STATE)
	{
		const string callsign = FlightPlan.GetCallsign();
		const string state = FlightPlan.GetGroundState();

		// departing: the intersections are no longer relevant (same as vSID)
		if (state == "DEPA") intersections.erase(callsign);

		// a request is fulfilled when its ground state is set (same rules as vSID)
		if (const auto pending = requests.find(callsign); pending != requests.end())
		{
			const string& type = pending->second.type;

			if ((state == "STUP" && type == "startup") ||
				(state == "PUSH" && type == "pushback") ||
				(state == "TAXI" && type == "taxi") ||
				(state == "DEPA" && (type == "departure" || type == "vfr")))
			{
				requests.erase(pending);
			}
		}
		return;
	}

	if (DataType != EuroScopePlugIn::CTR_DATA_TYPE_SCRATCH_PAD_STRING) return;

	sync.update(FlightPlan); // releases our own pending sync message when the echo arrives

	const string callsign = FlightPlan.GetCallsign();
	const string scratch = upperCopy(FlightPlan.GetControllerAssignedData().GetScratchPadString());

	if (const auto value = tagValue(scratch, REQ_TAG); value.has_value())
	{
		// "<type>/<time>" or "NONE/0"
		const size_t slash = value->find('/');
		if (slash != string::npos)
		{
			const string type = value->substr(0, slash);

			if (type == "NONE")
			{
				requests.erase(callsign);
			}
			else if (const RequestKind* kind = requestKindByType(type); kind != nullptr)
			{
				const string adep = FlightPlan.GetFlightPlanData().GetOrigin();

				if (activeAirports.contains(adep))
				{
					try
					{
						requests[callsign] = Request{ type, kind->letter, std::stoll(value->substr(slash + 1)), adep };
					}
					catch (const std::exception&) {}
				}
			}
		}
	}

	if (const auto value = tagValue(scratch, INT_TAG); value.has_value())
	{
		// "A_<INT>" = able, "S_<INT>" = cleared, "NONE" as name clears that one
		if (value->size() > 2 && (*value)[1] == '_' && ((*value)[0] == 'A' || (*value)[0] == 'S'))
		{
			const string name = value->substr(2);
			auto& entry = intersections[callsign];
			((*value)[0] == 'S' ? entry.second : entry.first) = (name == "NONE") ? string() : name;
			if (entry.first.empty() && entry.second.empty()) intersections.erase(callsign);
		}
	}
	if (const auto sender = tagValue(scratch, AUTO_TAG); sender.has_value())
	{
		onAutoMarker(FlightPlan, *sender);
	}
}

CRadarScreen* BeluxPlugin::OnRadarScreenCreated(const char* sDisplayName, bool NeedRadarContent, bool GeoReferenced,
                                                bool CanBeSaved, bool CanBeCreated)
{
	return new BeluxScreen(this);
}
BeluxPlugin* gpMyPlugin = NULL;

void __declspec (dllexport) EuroScopePlugInInit(EuroScopePlugIn::CPlugIn** ppPlugInInstance)
{
	// create the instance
	*ppPlugInInstance = gpMyPlugin = new BeluxPlugin();
}


//---EuroScopePlugInExit-----------------------------------------------

void __declspec (dllexport) EuroScopePlugInExit(void)
{
	// delete the instance
	delete gpMyPlugin;
}
