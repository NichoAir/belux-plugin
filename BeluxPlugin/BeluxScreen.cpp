#include "pch.h"
#include "BeluxScreen.hpp"
#include "BeluxPlugin.hpp"

BeluxScreen::BeluxScreen(BeluxPlugin* plugin) : plugin_(plugin)
{
	if (plugin_) plugin_->registerScreen(this);
}

BeluxScreen::~BeluxScreen()
{
	if (plugin_) plugin_->unregisterScreen(this);
}

void BeluxScreen::OnAsrContentToBeClosed(void)
{
	delete this;
}
