# Changelog

## 1.7.0 (previous release: 1.6.3)

### Added

#### Auto mode (`.belux auto [ICAO ...]`)
- New per-airport auto mode, modelled on vSID. `.belux auto` toggles all active departure airports, `.belux auto EBBR EBOS` toggles only the named ones.
- For every aircraft at an auto airport (checked every 5 seconds) the plugin calls the existing `ProcedureAssigner::process_flight_plan()`, so all existing rules stay in place (no change for aircraft with a clearance flag, tracked by someone else, or already carrying a SID in the route).
- **IFR only:** only flight plans with type `I` or `Y` are handled. VFR aircraft get neither a SID nor a squawk.
- **Squawk via CCAMS:** after processing, aircraft that have no squawk (empty, `0000` or `1234`) get one from CCAMS.
  - Existing squawks are never overwritten.
  - Aircraft must be on the ground (at most 1500 ft, at most 50 kt), correlated, and not tracked by another controller.
  - Requests are queued, at most one every 2 seconds, with a retry only after 60 seconds.
  - CCAMS is detected automatically (also when it is loaded after Belux).
  - Your current aircraft selection is restored after each request, so assigning squawks doesn't move your selection.
- **Lowest station only:**
  - Auto mode can't be started if another controller at the same airport has a lower **or equal** facility (DEL < GND < TWR < APP < CTR). Controllers are matched to airports by their `ICAO_` callsign prefix, ATIS/observers are ignored.
  - A running auto mode is switched off automatically as soon as a **lower** station connects, or when the airport is no longer active.
  - If two stations end up in auto mode on the same airport, the higher level backs off. On equal level the alphabetically later callsign backs off.
- Auto mode is switched off when the client disconnects. Requests, intersections and remembered states are kept; other controllers can re-send changes made in the meantime with `.belux sync`. After a reconnect, enable it again with `.belux auto`.
- **Stations covering several airports:** a station counts as controller of every airport in its `coverage` entry, not only of the one in its callsign. By default `EBBR_APP` (and `EBBR_*_APP`) covers EBBR, EBAW and EBCI. So a CTR such as `EBBU_W_CTR` can't auto-assign on those airports while the approach is online, and an approach that auto-assigns for EBCI sees `EBCI_TWR` as lower station.
- After a successful auto assignment a marker (`.BELUX_AUTO_<callsign>`) is sent to the other controllers, who get a chat notice.

#### Tag function "Auto SID + squawk"
- Does for **one aircraft** what auto mode does: assigns SID/RWY according to the SID assigner and requests a squawk from CCAMS. It works whether or not auto mode is active.
- Same rules as auto mode: IFR only, SID/RWY unchanged if one is already assigned / clearance given / tracked by someone else, squawk only if none is assigned and the aircraft is on the ground and not tracked by another controller.
- Works silently, what was done (or why nothing changed) is only shown in debug mode. No marker is sent to other controllers, so it doesn't make other stations leave auto mode.

#### Request tag items (shared between controllers)
- New tag items **Request** (request letter plus place in the queue, e.g. `S1`) and **Request Timer** (minutes since the request, e.g. `4m`), and the tag function **Request Menu**.
- Request types: IFR Clearance (`C`), Startup (`S`), Pushback (`P`), Taxi (`T`), Departure (`D`). VFR flight plans get **VFR** (`V`) instead of IFR Clearance. **No Req** removes a request.
- The queue position counts all earlier requests of the same type at the same airport.
- Requests are removed automatically when fulfilled: clearance flag set (IFR Clearance), ground state `STUP` (Startup), `PUSH` (Pushback), `TAXI` (Taxi), `DEPA` (Departure, VFR).
- The timer is grey, turns yellow after `caution_minutes` and red after `warning_minutes` (see config).
- A grey `-` is shown for aircraft on the ground at an active airport that have no request, so the item stays clickable.

#### Intersection tag items (runway intersections)
- Two independent tag items: **Intersection (able)** (yellow) and **Intersection (cleared)** (green).
- Two tag functions open a popup of the intersections configured for the aircraft's departure runway: **Select runway intersection as able** and **Set runway intersection**. The popup also has **Custom** (free text, up to 5 letters/digits) and **Clear** (clears only the item the popup was opened from).
- Values are cleared when the ground state becomes `DEPA` and on disconnect.
- A grey `-` placeholder is shown on the ground while nothing is selected.

#### Synchronisation between controllers
- New `BeluxSync`: scratch pad based message queue (same principle as vSID). A message is written to the scratch pad, the echo is awaited and the old scratch pad is restored, with a 0.5 second watchdog.
- Messages use the prefix `.BELUX_` so they don't collide with vSID: `.BELUX_REQ_<type>/<time>`, `.BELUX_INT_A_<name>` / `.BELUX_INT_S_<name>`, `.BELUX_AUTO_<callsign>`.
- New "burst" mode for messages without echo (ES ground states and clearance keywords): all strings are written in a row, then the old scratch pad is restored.
- Clearance flag and ground status changes (ES ground states and GroundRadar statuses `ONFREQ`, `DE-ICE`, `ST-UP`, `PUSH`, `TAXI`, `LINEUP`, `DEPA`, `TXIN`, `PARK`) are now remembered by the plugin.

#### Commands
- `.belux auto [ICAO ...]`: toggle auto mode.
- `.belux sync <ICAO ...>`: re-sends everything the other controllers might have missed, **only for the airports you name** (nothing is synced automatically; without an airport you get a hint):
  - clearance flags and ground statuses of aircraft on the ground (within 5 NM of origin, 60 kt or less, correlated, not tracked), like the "Sync" plugin,
  - pending requests,
  - intersections.
  - If the stored old scratch pad is itself a status keyword, it is not restored, so stale statuses aren't re-broadcast.
- Both commands are listed in `.belux help`.

#### Radar screen
- New minimal `BeluxScreen` (draws nothing). EuroScope only lets plugins call tag functions of other plugins through a radar screen, which CCAMS squawk assignment needs. **At least one ASR must be open for automatic squawks.** A chat message is shown once if no screen exists.

### Removed
- The old automatic RWY/SID assignment: the `rwy_sid_assigner` config flag, the `.belux rwysidon` / `.belux rwysidoff` commands and the automatic activation at startup. Use auto mode (`.belux auto`) or the "Auto SID + squawk" tag function instead.
- The `.belux force-sid` command and the "Assign RWY/SID" tag function (forced SID/RWY assignment). The "Auto SID + squawk" function replaces the tag function; it doesn't overwrite an existing SID. Tag item / function IDs of the other items are unchanged. `.belux fresh-sid` (clear the SID cache) stays.

### Changed
- `ProcessFlightPlans` skips invalid flight plans (previously `GetFlightPlanData()` was called on them).
- `GetHttpsRequest` updated for Boost 1.87: `boost::asio::io_service` is now `io_context` and `resolver::query` was replaced by `resolver.resolve(host, "https")`.
- Project: `BeluxSync.cpp/.hpp` and `BeluxScreen.cpp/.hpp` added to `BeluxPlugin.vcxproj` and its filters.

### Configuration (`belux_config.json`, all sections optional)

```json
"coverage": {
    "EBBR_APP": ["EBBR", "EBAW", "EBCI"],
    "EBBR_*_APP": ["EBBR", "EBAW", "EBCI"]
},
"requests": {
    "caution_minutes": 5,
    "warning_minutes": 10
},
"intersection": {
    "able_color": [255, 220, 0],
    "set_color": [0, 220, 0],
    "list": { "EBBR": { "25R": ["..."], "19": ["..."] } }
}
```

- `coverage`: stations that cover other airports than the one in their callsign (`*` works as wildcard in the callsign). If present it replaces the built-in default, which is the entry shown above.
- `requests`: minutes after which the request timer turns yellow / red. The defaults are guesses and may need adjusting.
- `intersection.list`: airport, then departure runway, then the intersections offered in the popup. There is no built-in list; without entries only **Custom** and **Clear** are offered.
- `.belux reload` re-reads both sections.

### Requirements and notes
- **CCAMS** must be loaded for automatic squawks. Function ID `871` ("auto assign squawk") is the one vSID uses.
- Build: Boost 1.87 libraries matching the toolset in use (for example `boost_json` and `boost_container` for `vc143`); the project needs the MFC component of the selected toolset.
- New tag items and functions (add them in the tag editor): Intersection (able), Intersection (cleared), Request, Request Timer, Set runway intersection, Select runway intersection as able, Request Menu, Auto SID + squawk.
- Belux and vSID can run side by side, since their scratch pad messages use different prefixes.
