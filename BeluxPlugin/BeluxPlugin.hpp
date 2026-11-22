#pragma once
#include "EuroScopePlugIn.h"
#include "BeluxGatePlanner.hpp"
#include "ProcedureAssigner.h"
#include "BeluxUtil.hpp"
#include "BeluxSync.hpp"
#include "BeluxScreen.hpp"
#include <chrono>
#include <optional>
#include <deque>
#include <time.h>
#include <vector>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/algorithm/string.hpp>

#include <rapidjson/document.h>
#include <rapidjson/istreamwrapper.h>
#include <rapidjson/writer.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/ostreamwrapper.h>



#define MY_PLUGIN_NAME      "Belux"
#define MY_PLUGIN_VERSION   "1.7.0"
#define MY_PLUGIN_DEVELOPER "Nicola Macoir, Stef Pletinck for Belux vACC"
#define MY_PLUGIN_COPYRIGHT "GPL v3"
#define MY_PLUGIN_VIEW_AVISO  "Belux vACC"

#ifndef AUTH_TOKEN
#pragma message("No AUTH_TOKEN found")
#define AUTH_SECRET "Placeholder"
#else
// Behold, fuckery to quote things
#define stringify_literal( x ) # x
#define stringify_expanded( x ) stringify_literal( x )
#define AUTH_SECRET stringify_expanded( AUTH_TOKEN )
#pragma message("AUTH_TOKEN is defined to: " AUTH_SECRET )
#endif

using namespace std;
using namespace EuroScopePlugIn;

enum TagDefinitions: int {
	item_gate_assign = 1,
	function_gate_refresh,
	item_mach_number,
	function_force_sid,
	item_proc_suggestion,
	vspeed,
	item_intersection_able,
	item_intersection_cleared,
	function_intersection_set,
	function_intersection_able,
	function_intersection_set_pick, // popup list entries / custom edit result, not registered as tag functions
	function_intersection_able_pick,
	item_request,
	item_request_timer,
	function_request_menu,
	function_request_pick, // popup list entries, not registered as tag function
};

/** Ground / GroundRadar statuses, same set as the "Sync" plugin tracks. */
enum class BeluxGroundStatus
{
	NoState,
	OnFreq,
	Stby, // GroundRadar "DE-ICE"
	Stup,
	Push,
	Taxi,
	Lineup,
	Takeoff,
	TaxiIn,
	Parked,
};

class BeluxPlugin :
	public EuroScopePlugIn::CPlugIn
{
public:
	BeluxPlugin();
	virtual ~BeluxPlugin();
	virtual void OnNewMetarReceived(const char* sStation, const char* sFullMetar);
	virtual void OnFlightPlanFlightPlanDataUpdate(CFlightPlan FlightPlan);
	virtual void OnFlightPlanDisconnect(CFlightPlan FlightPlan);
	virtual void OnGetTagItem(EuroScopePlugIn::CFlightPlan FlightPlan, EuroScopePlugIn::CRadarTarget RadarTarget, int ItemCode, int TagData, char sItemString[16], int* pColorCode, COLORREF* pRGB, double* pFontSize);
	virtual void OnFunctionCall(int FunctionId, const char* sItemString, POINT Pt, RECT Area);
	virtual bool OnCompileCommand(const char* sCommandLine);
	virtual void OnTimer(int Counter);
	virtual void OnAirportRunwayActivityChanged(void);
	virtual void OnFlightPlanControllerAssignedDataUpdate(CFlightPlan FlightPlan, int DataType);
	virtual CRadarScreen* OnRadarScreenCreated(const char* sDisplayName, bool NeedRadarContent, bool GeoReferenced,
	                                           bool CanBeSaved, bool CanBeCreated);

	void registerScreen(BeluxScreen* screen) { screens.insert(screen); }
	void unregisterScreen(BeluxScreen* screen) { screens.erase(screen); }


protected:
	unsigned int vspeed_threshold = 300;
	UINT8 on_gate_change = 2; // 2: blink. 1: quiet. 0: none
	bool function_fetch_gates = true;
	bool function_set_initial_climb = true;
	bool function_mach_visualisation = true;
	bool function_check_runway_and_sid = false;
	bool force_new_procedure = false;
	
	int timeout_value = 1000;

	set<string>* processed;
	set<string> activeAirports;

	BeluxGatePlanner gatePlanner;
	BeluxUtil utils;
	ProcedureAssigner* procedureAssigner;

	// ---- runway intersections (holding points) ---------------------------------------------------------------
	map<string, pair<string, string>> intersections; // callsign -> (able intersection, cleared intersection)
	map<string, map<string, vector<string>>> intersectionList; // icao -> rwy -> selectable intersections (config)
	COLORREF intersectionAbleColor = RGB(255, 220, 0);
	COLORREF intersectionSetColor = RGB(0, 220, 0);

	// ---- shared requests (IFR clearance, startup, pushback, taxi, ...) ---------------------------------------
	struct Request
	{
		string type; // clearance, startup, pushback, taxi, departure, vfr
		char letter = '?'; // shown in the tag item, e.g. S2 = 2nd in the startup queue
		long long time = 0; // request time (unix seconds)
		string adep;
	};
	map<string, Request> requests; // callsign -> pending request
	int request_caution_minutes = 5;
	int request_warning_minutes = 10;

	// ---- tracked clearance flags / ground states, replayed by .belux sync (like the "Sync" plugin) ---------
	// ES doesn't keep GroundRadar statuses, so every change has to be remembered when it arrives
	struct GroundTrack
	{
		bool cleared = false;
		optional<BeluxGroundStatus> status;
	};
	map<string, GroundTrack> groundTrack;

	// ---- auto mode -----------------------------------------------------------------------------------------
	set<string> autoAirports; // airports with active auto mode (always only airports where we are the lowest station)
	bool wasConnected = false; // to detect the moment the client disconnects
	bool autoStartupPending = false; // config "rwy_sid_assigner": enable as soon as we are connected as controller
	/** icao -> (callsign -> facility) of all other controllers whose callsign starts with that icao */
	map<string, map<string, int>> airportControllers;

	// ---- sync / squawk -------------------------------------------------------------------------------------
	BeluxSync sync;
	set<BeluxScreen*> screens;
	bool ccamsLoaded = false;
	bool noScreenWarned = false;
	deque<string> squawkQueue;
	map<string, chrono::steady_clock::time_point> squawkRequested;
	chrono::steady_clock::time_point lastSquawkTP{};

	map<string, vector<string>> activeDepRunways;
	map<string, vector<string>> activeArrRunways;

	map<string, int> QNH{{"EBLG", 0}, {"EBBR", 0}, {"EBOS", 0}};

	string GetHttpsRequest(string host, string uri, string request, bool expect_long_json);
	string GetGateInfo();
	string GetAirportInfo(const string& airport);
	string GetLatestPluginVersion();
	string SwapGate(string callsign, string gate);
	void ProcessMETAR(string airport, string metar);
	/**
	 * \brief Sets all active runways in the plugin properties
	 */
	void getActiveRunways();
	void ProcessFlightPlans();
	void FetchAndProcessGates();
	void versionCheck();
	void loadJSONconfig();

	// auto mode
	bool isMyselfController();
	void refreshControllers();
	bool hasLowerAtc(const string& icao, bool toActivate);
	bool enableAuto(const string& icao);
	void enableAutoAll();
	void applyStartupAuto();
	void handleDisconnect();
	void enforceLowestStation();
	bool isAutoActive(const string& icao) const { return autoAirports.contains(icao); }
	void autoProcess(CFlightPlan& fp, const string& adep);
	void onAutoMarker(CFlightPlan& fp, const string& sender);
	void handleAutoCommand(const string& commandLine);

	// squawk (CCAMS)
	void detectExternalPlugins();
	bool isGroundAndMine(CFlightPlan& fp);
	void queueSquawk(CFlightPlan& fp);
	void processSquawkQueue();
	bool callCcamsSquawk(const string& callsign);

	// intersections
	void openIntersectionMenu(CFlightPlan fp, RECT area, bool set);
	void applyIntersection(CFlightPlan fp, const string& name, bool set);

	// requests
	void openRequestMenu(CFlightPlan fp, RECT area);
	void applyRequest(CFlightPlan fp, const string& label);
	int requestPosition(const string& callsign) const;
	bool isIfr(CFlightPlan& fp);

	// sync
	void trackGroundState(CFlightPlan& fp, int dataType);
	void resyncStates(const set<string>& airports);

	void printDebugMessage(const string& function, const string& message);
	void printMessage(const string& topic, const string& message);
};

inline static bool startsWith(const char* pre, const char* str)
{
	size_t lenpre = strlen(pre), lenstr = strlen(str);
	return lenstr < lenpre ? false : strncmp(pre, str, lenpre) == 0;
};