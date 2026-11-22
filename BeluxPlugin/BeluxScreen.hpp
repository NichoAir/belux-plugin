#pragma once
#include "EuroScopePlugIn.h"

class BeluxPlugin;

/**
 * Minimal radar screen. The plugin had none so far, but ES only allows calling tag functions of other
 * plugins (CCAMS squawk assignment) through CRadarScreen::StartTagFunction.
 * It draws nothing and only registers itself at the plugin.
 */
class BeluxScreen : public EuroScopePlugIn::CRadarScreen
{
public:
	explicit BeluxScreen(BeluxPlugin* plugin);
	virtual ~BeluxScreen();

	virtual void OnAsrContentLoaded(bool Loaded) override {}
	virtual void OnAsrContentToBeClosed(void) override;

	/** Called by the plugin when it is destroyed before the screen. */
	void detach() { plugin_ = nullptr; }

private:
	BeluxPlugin* plugin_;
};
